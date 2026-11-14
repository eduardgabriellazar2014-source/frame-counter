#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

class $modify(FrameCounterLayer, PlayLayer) {
	struct Fields {
		CCLabelBMFont* label = nullptr;
		int lastFrame = -1;
	};

	bool init(GJGameLevel* level, bool dontCreateObjects) {
		if (!PlayLayer::init(level, dontCreateObjects)) return false;

		auto label = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
		label->setID("frame-counter"_spr);
		m_fields->label = label;

		// Added to the UI layer so it stays fixed on screen and ignores camera movement.
		m_uiLayer->addChild(label, 1000);

		this->applyLayout();
		return true;
	}

	// Positions / styles the label according to the mod settings.
	void applyLayout() {
		auto label = m_fields->label;
		if (!label) return;

		auto mod = Mod::get();
		bool enabled = mod->getSettingValue<bool>("enabled");
		label->setVisible(enabled);
		if (!enabled) return;

		auto corner = mod->getSettingValue<std::string>("corner");
		float scale = static_cast<float>(mod->getSettingValue<double>("scale"));
		int opacity = static_cast<int>(mod->getSettingValue<int64_t>("opacity"));

		label->setScale(scale);
		label->setOpacity(static_cast<GLubyte>(std::clamp(opacity, 0, 255)));

		auto winSize = CCDirector::get()->getWinSize();
		constexpr float margin = 6.f;

		bool right = corner.find("Right") != std::string::npos;
		bool top = corner.find("Top") != std::string::npos;

		label->setAnchorPoint({ right ? 1.f : 0.f, top ? 1.f : 0.f });
		label->setPosition({
			right ? winSize.width - margin : margin,
			top ? winSize.height - margin : margin
		});
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);

		auto label = m_fields->label;
		if (!label) return;

		auto mod = Mod::get();
		if (!mod->getSettingValue<bool>("enabled")) {
			label->setVisible(false);
			return;
		}

		this->applyLayout();

		// m_levelTime is part of the game state, so it is automatically
		// restored when loading practice mode checkpoints.
		int64_t fps = mod->getSettingValue<int64_t>("fps");
		int frame = static_cast<int>(m_gameState.m_levelTime * static_cast<double>(fps));
		if (frame < 0) frame = 0;

		if (frame != m_fields->lastFrame) {
			m_fields->lastFrame = frame;
			if (mod->getSettingValue<bool>("show-prefix")) {
				label->setString(fmt::format("Frame: {}", frame).c_str());
			} else {
				label->setString(fmt::format("{}", frame).c_str());
			}
		}
	}
};
