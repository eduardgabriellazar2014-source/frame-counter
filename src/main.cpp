#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <array>
#include <cmath>

using namespace geode::prelude;

namespace {
	constexpr int ROWS = 7;
	const char* ROW_NAMES[ROWS] = { "1", "2", "3", "4", "5-8", "9-16", "17+" };
	const ccColor3B ROW_COLORS[ROWS] = {
		{ 255, 90, 90 }, { 255, 150, 80 }, { 255, 210, 90 }, { 200, 235, 100 },
		{ 120, 225, 140 }, { 110, 200, 230 }, { 150, 160, 255 }
	};

	struct GapStats {
		std::array<int, ROWS> counts{};
		int clicks = 0;
		int gaps = 0;
		long long lastFrame = -1;
		long long lastGap = 0;
		double lastTime = -1.0;
		double lastMs = 0.0;
		double mean = 0.0;
		double m2 = 0.0;
		bool dirty = true;

		void reset() {
			counts.fill(0);
			clicks = 0;
			gaps = 0;
			lastFrame = -1;
			lastGap = 0;
			lastTime = -1.0;
			lastMs = 0.0;
			mean = 0.0;
			m2 = 0.0;
			dirty = true;
		}

		double sigma() const {
			return gaps > 1 ? std::sqrt(m2 / gaps) : 0.0;
		}
	};

	GapStats g_stats;

	int rowFor(long long gap) {
		if (gap <= 4) return static_cast<int>(gap) - 1;
		if (gap <= 8) return 4;
		if (gap <= 16) return 5;
		return 6;
	}
}

class $modify(GapPlayerObject, PlayerObject) {
	bool pushButton(PlayerButton button) {
		bool result = PlayerObject::pushButton(button);

		// Only count jump presses by player 1 inside a real level (not the editor).
		if (button != PlayerButton::Jump) return result;
		auto pl = PlayLayer::get();
		if (!pl || this != pl->m_player1) return result;
		if (!Mod::get()->getSettingValue<bool>("enabled")) return result;

		long long frame = static_cast<long long>(pl->m_gameState.m_currentProgress);
		double t = pl->m_gameState.m_levelTime;

		g_stats.clicks++;
		if (g_stats.lastFrame >= 0) {
			long long gap = frame - g_stats.lastFrame;
			if (gap > 0) {
				g_stats.counts[rowFor(gap)]++;
				g_stats.lastGap = gap;
				g_stats.lastMs = (t - g_stats.lastTime) * 1000.0;
				g_stats.gaps++;
				double delta = static_cast<double>(gap) - g_stats.mean;
				g_stats.mean += delta / g_stats.gaps;
				g_stats.m2 += delta * (static_cast<double>(gap) - g_stats.mean);
			}
		}
		g_stats.lastFrame = frame;
		g_stats.lastTime = t;
		g_stats.dirty = true;
		return result;
	}
};

class $modify(GapPlayLayer, PlayLayer) {
	struct Fields {
		std::array<CCLabelBMFont*, ROWS> rows{};
		CCLabelBMFont* info1 = nullptr;
		CCLabelBMFont* info2 = nullptr;
	};

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		g_stats.reset();

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
		g_stats.reset();
		this->refreshLabels();
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		this->refreshLabels();
	}

	void refreshLabels() {
		auto mod = Mod::get();
		bool enabled = mod->getSettingValue<bool>("enabled");
		bool showInfo = mod->getSettingValue<bool>("show-info");
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
			if (g_stats.dirty) {
				label->setString(fmt::format("{}f: {}", ROW_NAMES[i], g_stats.counts[i]).c_str());
			}
		}

		float y2 = y - ROWS * step - step * 0.4f;
		double t = m_gameState.m_levelTime;
		double cps = t > 0.5 ? g_stats.clicks / t : 0.0;

		if (m_fields->info1) {
			m_fields->info1->setVisible(enabled && showInfo);
			m_fields->info1->setScale(scale);
			m_fields->info1->setOpacity(op);
			m_fields->info1->setPosition({ x, y2 });
			if (g_stats.dirty) {
				m_fields->info1->setString(
					fmt::format("last {}f / {:.2f}ms", g_stats.lastGap, g_stats.lastMs).c_str());
			}
		}
		if (m_fields->info2) {
			m_fields->info2->setVisible(enabled && showInfo);
			m_fields->info2->setScale(scale);
			m_fields->info2->setOpacity(op);
			m_fields->info2->setPosition({ x, y2 - step });
			m_fields->info2->setString(
				fmt::format("sd {:.2f}f  {:.1f}/s  n={}", g_stats.sigma(), cps, g_stats.clicks).c_str());
		}

		g_stats.dirty = false;
	}
};
