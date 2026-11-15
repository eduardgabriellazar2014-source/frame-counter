#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
	// One recorded jump input: the physics frame it happened on, press/release, which player.
	struct Ev {
		long long step;
		bool down;
		bool p2;
	};

	constexpr int ROWS = 6;
	const char* ROW_NAMES[ROWS] = { "1f", "2f", "3f", "4f", "5f+", "fail" };
	const ccColor3B ROW_COLORS[ROWS] = {
		{ 255, 90, 90 }, { 255, 160, 80 }, { 255, 225, 90 },
		{ 150, 230, 110 }, { 110, 200, 235 }, { 170, 170, 170 }
	};

	enum class Phase { Idle, Restart, Advance, Snapshot, BranchStart, Branch, Restore, Done };

	struct Scan {
		bool active = false;
		bool stepping = false;
		Phase phase = Phase::Idle;

		std::vector<Ev> ev;        // recorded inputs, sorted by frame
		std::vector<int> pressEv;  // indices into ev of every player 1 press
		int rangeS = 12;
		double stepDt = 1.0 / 240.0;

		int click = 0;
		size_t baseIdx = 0;
		CheckpointObject* snap = nullptr;
		long long snapProgress = 0;
		size_t snapBaseIdx = 0;

		int shift = 0;
		long long pStep = 0;
		long long horizon = 0;
		std::vector<Ev> local;
		size_t localIdx = 0;
		bool died = false;
		bool reachedEnd = false;
		int idleTicks = 0;
		long long lastProg = -1;
		std::vector<char> survive;

		std::vector<int> windows;
		std::vector<char> capped;
		std::string text;
	};
	Scan g_scan;

	struct Results {
		bool ready = false;
		std::vector<int> windows;
		std::vector<char> capped;
		std::array<int, ROWS> counts{};
		int measured = 0;
		double mean = 0.0;
		double sd = 0.0;
	};
	Results g_res;

	std::string g_msg;
	std::string g_levelName;
	bool g_dirty = true;

	std::vector<Ev> g_rec;    // inputs of the attempt that is being played
	std::vector<Ev> g_saved;  // inputs of the last attempt that finished the level
	int g_pressCounter = 0;
	bool g_hold[2] = { false, false };

	void computeStats() {
		g_res.counts.fill(0);
		double sum = 0.0, sq = 0.0;
		int n = 0;
		for (int w : g_res.windows) {
			int row = w <= 0 ? 5 : (w >= 5 ? 4 : w - 1);
			g_res.counts[row]++;
			if (w > 0) {
				n++;
				sum += w;
				sq += static_cast<double>(w) * w;
			}
		}
		g_res.measured = n;
		g_res.mean = n > 0 ? sum / n : 0.0;
		double var = n > 0 ? sq / n - g_res.mean * g_res.mean : 0.0;
		g_res.sd = var > 0.0 ? std::sqrt(var) : 0.0;
	}

	long long progressOf(PlayLayer* pl) {
		return static_cast<long long>(pl->m_gameState.m_currentProgress);
	}

	// Advances the real game by exactly one physics step. The update hook lets
	// this call through to the original game code because "stepping" is set.
	void stepGame(PlayLayer* pl) {
		g_scan.stepping = true;
		static_cast<GJBaseGameLayer*>(pl)->update(static_cast<float>(g_scan.stepDt));
		g_scan.stepping = false;
	}

	void applyEvent(PlayLayer* pl, Ev const& e) {
		PlayerObject* player = e.p2 ? pl->m_player2 : pl->m_player1;
		if (!player) return;
		if (e.down) player->pushButton(PlayerButton::Jump);
		else player->releaseButton(PlayerButton::Jump);
	}

	void injectDue(PlayLayer* pl, std::vector<Ev> const& v, size_t& idx) {
		long long prog = progressOf(pl);
		while (idx < v.size() && v[idx].step <= prog) {
			applyEvent(pl, v[idx]);
			idx++;
		}
	}

	long long pressStep(int click) {
		return g_scan.ev[g_scan.pressEv[click]].step;
	}

	long long snapStepFor(int click) {
		return std::max<long long>(0, pressStep(click) - g_scan.rangeS - 1);
	}

	void abortScan(PlayLayer* pl, std::string const& why) {
		g_scan.active = false;
		g_scan.phase = Phase::Idle;
		if (g_scan.snap) {
			g_scan.snap->release();
			g_scan.snap = nullptr;
		}
		g_msg = why;
		g_dirty = true;
		log::warn("window scan aborted: {}", why);
		if (pl) pl->resetLevelFromStart();
	}

	void trackIdle(PlayLayer* pl) {
		long long p = progressOf(pl);
		if (p == g_scan.lastProg) g_scan.idleTicks++;
		else {
			g_scan.idleTicks = 0;
			g_scan.lastProg = p;
		}
	}

	// Builds the list of inputs for one test: the clicked press and its release
	// (for every player that pressed on that frame) are moved by "shift" frames.
	void buildLocal(int shift) {
		auto& ev = g_scan.ev;
		auto& local = g_scan.local;
		local.clear();

		std::vector<char> moved(ev.size(), 0);
		for (int who = 0; who < 2; who++) {
			for (size_t k = g_scan.snapBaseIdx; k < ev.size(); k++) {
				if (ev[k].down && ev[k].step == g_scan.pStep && ev[k].p2 == (who == 1)) {
					moved[k] = 1;
					for (size_t r = k + 1; r < ev.size(); r++) {
						if (!ev[r].down && ev[r].p2 == ev[k].p2) {
							moved[r] = 1;
							break;
						}
					}
					break;
				}
			}
		}

		long long limit = g_scan.horizon + g_scan.rangeS + 8;
		for (size_t k = g_scan.snapBaseIdx; k < ev.size(); k++) {
			if (ev[k].step > limit) break;
			Ev e = ev[k];
			if (moved[k]) e.step += shift;
			local.push_back(e);
		}
		std::stable_sort(local.begin(), local.end(), [](Ev const& a, Ev const& b) {
			return a.step < b.step;
		});
	}

	// Turns the survive/fail list of one click into its window size: the length of
	// the run of surviving shifts that is closest to the original timing.
	void finishClick() {
		int S = g_scan.rangeS;
		int n = 2 * S + 1;
		auto& sv = g_scan.survive;
		int bestLen = 0;
		bool bestCap = false;
		int bestDist = 1000000;
		int i = 0;
		while (i < n) {
			if (!sv[i]) {
				i++;
				continue;
			}
			int j = i;
			while (j + 1 < n && sv[j + 1]) j++;
			int len = j - i + 1;
			int lo = i - S, hi = j - S;
			int dist = (lo <= 0 && hi >= 0) ? 0 : std::min(std::abs(lo), std::abs(hi));
			if (dist < bestDist || (dist == bestDist && len > bestLen)) {
				bestDist = dist;
				bestLen = len;
				bestCap = (i == 0 || j == n - 1);
			}
			i = j + 1;
		}
		g_scan.windows.push_back(bestLen);
		g_scan.capped.push_back(bestCap ? 1 : 0);

		std::string line = fmt::format("click {} frame {} window {}{} | ", g_scan.click, g_scan.pStep, bestLen, bestCap ? "+" : "");
		for (int k = 0; k < n; k++) line += sv[k] ? 'o' : '.';
		g_scan.text += line + "\n";
		g_dirty = true;
	}

	void writeLog() {
		try {
			auto path = Mod::get()->getSaveDir() / "scan.txt";
			std::ofstream f(path);
			f << "range +-" << g_scan.rangeS << " frames, o = survived, . = died\n";
			f << g_scan.text;
		} catch (...) {
		}
	}

	void finishScan(PlayLayer* pl) {
		g_res.windows = g_scan.windows;
		g_res.capped = g_scan.capped;
		computeStats();
		g_res.ready = true;
		g_msg.clear();
		g_dirty = true;
		writeLog();
		log::info("window scan finished: {} clicks", g_res.windows.size());

		g_scan.active = false;
		g_scan.phase = Phase::Idle;
		if (g_scan.snap) {
			g_scan.snap->release();
			g_scan.snap = nullptr;
		}
		pl->resetLevelFromStart();
	}

	// One small unit of scan work. Called many times per real frame.
	void scanTick(PlayLayer* pl) {
		int nPress = static_cast<int>(g_scan.pressEv.size());
		switch (g_scan.phase) {
			case Phase::Restart: {
				pl->resetLevelFromStart();
				g_scan.died = false;
				g_scan.reachedEnd = false;
				g_scan.baseIdx = 0;
				g_scan.click = 0;
				g_scan.idleTicks = 0;
				g_scan.lastProg = -1;
				pl->m_extraDelta = 0.0;
				g_scan.phase = Phase::Advance;
				break;
			}
			case Phase::Advance: {
				if (g_scan.died) {
					abortScan(pl, fmt::format("scan stopped: replay died at frame {} (turn your bot off and try again)", progressOf(pl)));
					break;
				}
				if (progressOf(pl) >= snapStepFor(g_scan.click)) {
					g_scan.phase = Phase::Snapshot;
					break;
				}
				injectDue(pl, g_scan.ev, g_scan.baseIdx);
				stepGame(pl);
				trackIdle(pl);
				if (g_scan.idleTicks > 5000) abortScan(pl, "scan stopped: the game stopped advancing");
				break;
			}
			case Phase::Snapshot: {
				CheckpointObject* c = pl->createCheckpoint();
				if (!c) {
					abortScan(pl, "scan stopped: could not save the game state");
					break;
				}
				c->retain();
				if (g_scan.snap) g_scan.snap->release();
				g_scan.snap = c;
				g_scan.snapProgress = progressOf(pl);
				g_scan.snapBaseIdx = g_scan.baseIdx;

				g_scan.pStep = pressStep(g_scan.click);
				g_scan.horizon = (g_scan.click + 1 < nPress)
					? pressStep(g_scan.click + 1)
					: g_scan.pStep + 600;
				g_scan.horizon = std::min(g_scan.horizon, g_scan.pStep + 960);

				g_scan.shift = -g_scan.rangeS;
				g_scan.survive.assign(2 * g_scan.rangeS + 1, 0);
				g_scan.phase = Phase::BranchStart;
				g_dirty = true;
				break;
			}
			case Phase::BranchStart: {
				if (g_scan.shift > g_scan.rangeS) {
					finishClick();
					g_scan.phase = Phase::Restore;
					break;
				}
				long long newStep = g_scan.pStep + g_scan.shift;
				if (newStep < 0 || newStep >= g_scan.horizon) {
					g_scan.shift++;
					break;
				}
				pl->loadFromCheckpoint(g_scan.snap);
				pl->m_extraDelta = 0.0;
				pl->m_levelEndAnimationStarted = false;
				g_scan.died = false;
				g_scan.reachedEnd = false;
				g_scan.idleTicks = 0;
				g_scan.lastProg = -1;
				buildLocal(g_scan.shift);
				g_scan.localIdx = 0;
				g_scan.phase = Phase::Branch;
				break;
			}
			case Phase::Branch: {
				int idx = g_scan.shift + g_scan.rangeS;
				if (g_scan.died) {
					g_scan.survive[idx] = 0;
					g_scan.shift++;
					g_scan.phase = Phase::BranchStart;
					break;
				}
				if (g_scan.reachedEnd || pl->m_levelEndAnimationStarted || progressOf(pl) >= g_scan.horizon) {
					g_scan.survive[idx] = 1;
					g_scan.shift++;
					g_scan.phase = Phase::BranchStart;
					break;
				}
				injectDue(pl, g_scan.local, g_scan.localIdx);
				stepGame(pl);
				trackIdle(pl);
				if (g_scan.idleTicks > 5000) abortScan(pl, "scan stopped: the game stopped advancing");
				break;
			}
			case Phase::Restore: {
				pl->loadFromCheckpoint(g_scan.snap);
				pl->m_extraDelta = 0.0;
				pl->m_levelEndAnimationStarted = false;
				g_scan.baseIdx = g_scan.snapBaseIdx;
				g_scan.died = false;
				g_scan.reachedEnd = false;
				g_scan.click++;
				g_scan.phase = (g_scan.click >= nPress) ? Phase::Done : Phase::Advance;
				break;
			}
			case Phase::Done: {
				finishScan(pl);
				break;
			}
			default: {
				g_scan.active = false;
				break;
			}
		}
	}

	void runScanSlice(PlayLayer* pl) {
		auto start = std::chrono::steady_clock::now();
		int iter = 0;
		while (g_scan.active) {
			scanTick(pl);
			if ((++iter & 31) == 0) {
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - start).count();
				if (ms >= 12) break;
			}
		}
		g_dirty = true;
	}

	void startScan(PlayLayer* pl) {
		if (g_scan.active) return;
		if (g_saved.empty()) {
			g_msg = "finish the level with your bot first";
			g_dirty = true;
			return;
		}
		Scan s;
		s.ev = g_saved;
		std::stable_sort(s.ev.begin(), s.ev.end(), [](Ev const& a, Ev const& b) {
			return a.step < b.step;
		});
		for (size_t i = 0; i < s.ev.size(); i++) {
			if (s.ev[i].down && !s.ev[i].p2) s.pressEv.push_back(static_cast<int>(i));
		}
		if (s.pressEv.empty()) {
			g_msg = "no clicks were recorded";
			g_dirty = true;
			return;
		}
		auto mod = Mod::get();
		s.rangeS = static_cast<int>(mod->getSettingValue<int64_t>("range"));
		int64_t tps = mod->getSettingValue<int64_t>("tps");
		s.stepDt = 1.0 / static_cast<double>(std::max<int64_t>(1, tps));
		s.active = true;
		s.phase = Phase::Restart;
		g_scan = s;
		g_res = Results{};
		g_msg.clear();
		g_dirty = true;
		log::info("window scan started: {} clicks, range {}", g_scan.pressEv.size(), g_scan.rangeS);
	}
}

class $modify(ScanPlayLayer, PlayLayer) {
public:
	struct Fields {
		std::array<CCLabelBMFont*, ROWS> rows{};
		CCLabelBMFont* info1 = nullptr;
		CCLabelBMFont* info2 = nullptr;
		CCLabelBMFont* popup = nullptr;
	};

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		if (g_scan.active) {
			g_scan.active = false;
			g_scan.phase = Phase::Idle;
			if (g_scan.snap) {
				g_scan.snap->release();
				g_scan.snap = nullptr;
			}
		}
		std::string name = level ? std::string(level->m_levelName) : std::string();
		if (name != g_levelName) {
			g_levelName = name;
			g_saved.clear();
			g_res = Results{};
			g_msg.clear();
		}
		g_rec.clear();
		g_pressCounter = 0;
		g_hold[0] = g_hold[1] = false;
		g_dirty = true;

		for (int i = 0; i < ROWS; i++) {
			auto label = CCLabelBMFont::create("", "bigFont.fnt");
			label->setAnchorPoint({ 0.f, 1.f });
			label->setColor(ROW_COLORS[i]);
			m_uiLayer->addChild(label, 1000);
			m_fields->rows[i] = label;
		}
		auto a = CCLabelBMFont::create("", "bigFont.fnt");
		a->setAnchorPoint({ 0.f, 1.f });
		a->setColor({ 210, 210, 210 });
		m_uiLayer->addChild(a, 1000);
		m_fields->info1 = a;

		auto b = CCLabelBMFont::create("", "bigFont.fnt");
		b->setAnchorPoint({ 0.f, 1.f });
		b->setColor({ 210, 210, 210 });
		m_uiLayer->addChild(b, 1000);
		m_fields->info2 = b;

		this->refreshLabels();
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		if (!g_scan.active) {
			g_rec.clear();
			g_pressCounter = 0;
			g_hold[0] = g_hold[1] = false;
		}
		g_dirty = true;
		this->refreshLabels();
	}

	void levelComplete() {
		if (g_scan.active) {
			g_scan.reachedEnd = true;
			return;
		}
		g_saved = g_rec;
		PlayLayer::levelComplete();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (g_scan.active) {
			g_scan.died = true;
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		if (!g_scan.stepping) this->refreshLabels();
	}

	void showWindowPopup(int idx) {
		if (!g_res.ready || idx < 0 || idx >= static_cast<int>(g_res.windows.size())) return;
		if (!Mod::get()->getSettingValue<bool>("show-popup")) return;
		auto popup = m_fields->popup;
		if (!popup) {
			popup = CCLabelBMFont::create("", "bigFont.fnt");
			popup->setScale(0.5f);
			m_objectLayer->addChild(popup, 1000);
			m_fields->popup = popup;
		}
		int w = g_res.windows[idx];
		bool cap = g_res.capped[idx] != 0;
		popup->setString(w > 0 ? fmt::format("{}{}", w, cap ? "+" : "").c_str() : "?");
		popup->setPosition(m_player1->getPosition() + CCPoint{ 35.f, 35.f });
		popup->stopAllActions();
		popup->setOpacity(255);
		popup->runAction(CCSequence::create(CCDelayTime::create(0.45f), CCFadeOut::create(0.25f), nullptr));
	}

	void refreshLabels() {
		auto mod = Mod::get();
		bool enabled = mod->getSettingValue<bool>("enabled");
		float scale = static_cast<float>(mod->getSettingValue<double>("scale"));
		int opacity = static_cast<int>(mod->getSettingValue<int64_t>("opacity"));
		GLubyte op = static_cast<GLubyte>(std::clamp(opacity, 0, 255));

		auto winSize = CCDirector::get()->getWinSize();
		float x = 6.f;
		float y = winSize.height - 8.f;
		float step = scale * 42.f;

		for (int i = 0; i < ROWS; i++) {
			auto label = m_fields->rows[i];
			if (!label) continue;
			label->setVisible(enabled);
			label->setScale(scale);
			label->setOpacity(op);
			label->setPosition({ x, y - i * step });
			if (g_dirty) {
				label->setString(fmt::format("{}: {}", ROW_NAMES[i], g_res.counts[i]).c_str());
			}
		}

		std::string line1, line2;
		if (g_scan.active) {
			line1 = fmt::format("scanning {}/{}", g_scan.click + 1, g_scan.pressEv.size());
			line2 = "keep the game open";
		} else if (!g_msg.empty()) {
			line1 = g_msg;
		} else if (g_res.ready) {
			line1 = fmt::format("clicks {}  sd {:.2f}f", g_res.windows.size(), g_res.sd);
			line2 = fmt::format("avg {:.2f}f", g_res.mean);
		} else {
			line1 = "no scan yet";
			line2 = "pause, then Scan Windows";
		}

		float y2 = y - ROWS * step - step * 0.4f;
		auto setInfo = [&](CCLabelBMFont* label, std::string const& text, float posY) {
			if (!label) return;
			label->setVisible(enabled);
			label->setScale(scale);
			label->setOpacity(op);
			label->setPosition({ x, posY });
			if (g_dirty) label->setString(text.c_str());
		};
		setInfo(m_fields->info1, line1, y2);
		setInfo(m_fields->info2, line2, y2 - step);

		g_dirty = false;
	}
};

class $modify(ScanGameLayer, GJBaseGameLayer) {
	void update(float dt) {
		if (g_scan.active && !g_scan.stepping) {
			auto pl = PlayLayer::get();
			if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
				runScanSlice(pl);
				static_cast<ScanPlayLayer*>(pl)->refreshLabels();
				return;
			}
		}
		GJBaseGameLayer::update(dt);
	}
};

namespace {
	// Records jump inputs of the attempt that is being played (for example by a bot).
	void onInput(PlayerObject* obj, PlayerButton button, bool down) {
		if (button != PlayerButton::Jump) return;
		if (g_scan.active) return;
		auto pl = PlayLayer::get();
		if (!pl) return;
		bool p1 = obj == pl->m_player1;
		bool p2 = obj == pl->m_player2;
		if (!p1 && !p2) return;
		int who = p2 ? 1 : 0;
		if (g_hold[who] == down) return;
		g_hold[who] = down;

		g_rec.push_back({ progressOf(pl), down, p2 });
		if (down && p1) {
			int idx = g_pressCounter++;
			static_cast<ScanPlayLayer*>(pl)->showWindowPopup(idx);
		}
	}
}

class $modify(ScanPlayerObject, PlayerObject) {
	bool pushButton(PlayerButton button) {
		bool result = PlayerObject::pushButton(button);
		onInput(this, button, true);
		return result;
	}

	bool releaseButton(PlayerButton button) {
		bool result = PlayerObject::releaseButton(button);
		onInput(this, button, false);
		return result;
	}
};

class $modify(ScanPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		auto winSize = CCDirector::get()->getWinSize();
		auto menu = CCMenu::create();
		auto sprite = ButtonSprite::create("Scan Windows");
		auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(ScanPauseLayer::onScanWindows));
		menu->addChild(button);
		menu->setPosition({ winSize.width / 2.f, 28.f });
		this->addChild(menu);
	}

	void onScanWindows(CCObject*) {
		auto pl = PlayLayer::get();
		if (pl) startScan(pl);
		this->onResume(nullptr);
	}
};
