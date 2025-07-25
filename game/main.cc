#include "backgrounds.hh"
#include "chrono.hh"
#include "config.hh"
#include "controllers.hh"
#include "database.hh"
#include "engine.hh"
#include "fs.hh"
#include "graphic/glutil.hh"
#include "i18n.hh"
#include "log.hh"
#include "platform.hh"
#include "profiler.hh"
#include "screen.hh"
#include "songs.hh"
#include "graphic/window.hh"
#include "webcam.hh"
#include "webserver.hh"

// Screens
#include "screen_intro.hh"
#include "screen_songs.hh"
#include "screen_sing.hh"
#include "screen_practice.hh"
#include "screen_audiodevices.hh"
#include "screen_paths.hh"
#include "screen_players.hh"
#include "screen_playlist.hh"

#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <SDL_keyboard.h>
#include <SDL_scancode.h>

#include <cstdlib>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Disable main level exception handling for debug builds (because gdb cannot properly catch throwing otherwise)
#define RUNTIME_ERROR std::runtime_error
#define EXCEPTION std::exception

bool g_take_screenshot = false;

static void checkEvents(Game& gm, Time eventTime) {
	Window& window = gm.window();
	SDL_Event event;
	while (SDL_PollEvent(&event) == 1) {
		// Let the navigation system grab any and all SDL events
		gm.controllers.pushEvent(event, eventTime);
		auto type = event.type;
		if (type == SDL_WINDOWEVENT) window.event(event.window.event, event.window.data1, event.window.data2);
		if (type == SDL_QUIT) gm.finished();
		if (type == SDL_KEYDOWN) {
			SDL_Scancode key  = event.key.keysym.scancode;
			std::uint16_t mod = event.key.keysym.mod;
			bool altEnter = (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) && mod & KMOD_ALT;  // Alt+Enter
			bool modF = key == SDL_SCANCODE_F && mod & KMOD_CTRL && mod & KMOD_GUI;  // MacOS Ctrl+Cmd+F
			if (altEnter || modF || key == SDL_SCANCODE_F11) {
				config["graphic/fullscreen"].b() = !config["graphic/fullscreen"].b();
				continue; // Already handled here...
			}
			if (key == SDL_SCANCODE_PRINTSCREEN || (key == SDL_SCANCODE_F12 && (mod & Platform::shortcutModifier()))) {
				g_take_screenshot = true;
				continue; // Already handled here...
			}
			if (key == SDL_SCANCODE_F4 && mod & KMOD_ALT) {
				gm.finished();
				continue; // Already handled here...
			}
		}
		// Screens always receive SDL events that were not already handled here
		gm.getCurrentScreen()->manageEvent(event);
	}
	for (input::NavEvent event; gm.controllers.getNav(event); ) {
		input::NavButton nav = event.button;
		// Volume control
		if (nav == input::NavButton::VOLUME_UP || nav == input::NavButton::VOLUME_DOWN) {
			std::string curS = gm.getCurrentScreen()->getName();
			// Pick proper setting
			std::string which_vol = (curS == "Sing" || curS == "Practice")
			  ? "audio/music_volume" : "audio/preview_volume";
			// Adjust value
			if (nav == input::NavButton::VOLUME_UP) ++config[which_vol]; else --config[which_vol];
			// Show message
			gm.flashMessage(config[which_vol].getShortDesc() + ": " + config[which_vol].getValue());
			continue; // Already handled here...
		}
		// If a dialog is open, any nav event will close it
		if (gm.isDialogOpen()) { gm.closeDialog(); }
		// Let the current screen handle other events
		gm.getCurrentScreen()->manageEvent(event);
	}

	// Need to toggle full screen mode or adjust resolution?
	window.resize();
}

void mainLoop(std::string const& songlist) {
	Window window{};
	SpdLogger::info(LogSystem::LOGGER, "Loading assets...");
	TranslationEngine localization;
	TextureLoader m_loader;
	Backgrounds backgrounds;
	Database database(PathCache::getConfigDir() / "database.xml");
	Songs songs(database, songlist);
	loadFonts();

	window.start();
	Game gm(window);
	WebServer server(gm, songs);

	auto& audio = gm.getAudio();

	// Load audio samples
	gm.loading(_("Loading audio samples..."), 0.5f);
	audio.loadSample("drum bass", findFile("sounds/drum_bass.ogg"));
	audio.loadSample("drum snare", findFile("sounds/drum_snare.ogg"));
	audio.loadSample("drum hi-hat", findFile("sounds/drum_hi-hat.ogg"));
	audio.loadSample("drum tom1", findFile("sounds/drum_tom1.ogg"));
	audio.loadSample("drum cymbal", findFile("sounds/drum_cymbal.ogg"));
	//audio.loadSample("drum tom2", findFile("sounds/drum_tom2.ogg"));
	audio.loadSample("guitar fail1", findFile("sounds/guitar_fail1.ogg"));
	audio.loadSample("guitar fail2", findFile("sounds/guitar_fail2.ogg"));
	audio.loadSample("guitar fail3", findFile("sounds/guitar_fail3.ogg"));
	audio.loadSample("guitar fail4", findFile("sounds/guitar_fail4.ogg"));
	audio.loadSample("guitar fail5", findFile("sounds/guitar_fail5.ogg"));
	audio.loadSample("guitar fail6", findFile("sounds/guitar_fail6.ogg"));
	audio.loadSample("notice.ogg",findFile("notice.ogg"));
	// Load screens
	gm.loading(_("Creating screens..."), 0.7f);
	gm.addScreen(std::make_unique<ScreenIntro>(gm, "Intro", audio));
	gm.addScreen(std::make_unique<ScreenSongs>(gm, "Songs", audio, songs, database));
	gm.addScreen(std::make_unique<ScreenSing>(gm, "Sing", audio, database, backgrounds));
	gm.addScreen(std::make_unique<ScreenPractice>(gm, "Practice", audio));
	gm.addScreen(std::make_unique<ScreenAudioDevices>(gm, "AudioDevices", audio));
	gm.addScreen(std::make_unique<ScreenPaths>(gm, "Paths", audio, songs));
	gm.addScreen(std::make_unique<ScreenPlayers>(gm, "Players", audio, database));
	gm.addScreen(std::make_unique<ScreenPlaylist>(gm, "Playlist", audio, songs, backgrounds));
	gm.activateScreen("Intro");
	gm.loading(_("Entering main menu..."), 0.8f);
	gm.updateScreen();  // exit/enter, any exception is fatal error
	gm.loading(_("Loading complete!"), 1.0f);
	// Main loop
	auto time = Clock::now();
	unsigned frames = 0;
	SpdLogger::info(LogSystem::LOGGER, "Assets loaded, entering main loop.");
	while (!gm.isFinished()) {
		Profiler prof("mainloop");
		bool benchmarking = config["graphic/fps"].b();
		if (songs.doneLoading == true && songs.displayedAlert == false) {
			gm.dialog(fmt::format(_("Done Loading!\n Loaded {0} songs."), songs.loadedSongs()));
			songs.displayedAlert = true;
		}
		if (g_take_screenshot) {
			try {
				window.screenshot();
				gm.flashMessage(_("Screenshot taken!"));
			} catch (EXCEPTION& e) {
				SpdLogger::error(LogSystem::IMAGE, "Screenshot failed, exception={}", e.what());
				gm.flashMessage(_("Screenshot failed!"));
			}
			g_take_screenshot = false;
		}
		gm.updateScreen();  // exit/enter, any exception is fatal error
		if (benchmarking) prof("misc");
		try {
			window.blank();
			// Draw
			window.render(gm, [&gm]{ gm.drawScreen(); });
			if (benchmarking) { glFinish(); prof("draw"); }
			// Display (and wait until next frame)
			window.swap();
			if (benchmarking) { glFinish(); prof("swap"); }
			updateTextures();
			gm.prepareScreen();
			if (benchmarking) { glFinish(); prof("textures"); }
			if (benchmarking) {
				++frames;
				if (Clock::now() - time > 1s) {
					gm.flashMessage(fmt::format("{} FPS", frames));
					time += 1s;
					frames = 0;
				}
			} else {
				std::this_thread::sleep_until(time + 10ms); // Max 100 FPS
				time = Clock::now();
				frames = 0;
			}
			if (benchmarking) prof("fpsctrl");
			// Process events for the next frame
			auto eventTime = Clock::now();
			gm.controllers.process(eventTime);
			checkEvents(gm, eventTime);
			if (benchmarking) prof("events");
			} catch (RUNTIME_ERROR& e) {
				SpdLogger::error(LogSystem::LOGGER, "Caught error, exception={}", e.what());
				gm.flashMessage(std::string("ERROR: ") + e.what());
		}
	}

	writeConfig(gm);
}

/// Simple test utility to make mapping of joystick buttons/axes easier
void jstestLoop() {
	try {
		config["graphic/fullscreen"].b() = false;
		config["graphic/window_width"].i() = 640;
		config["graphic/window_height"].i() = 360;
		
		Window window{};
		window.start();
		input::Controllers controllers;
		controllers.enableEvents(true);
		// Main loop
		int oldjoy = -1, oldaxis = -1, oldvalue = -1;
		while (true) {
			SDL_Event e;
			while(SDL_PollEvent(&e) == 1) {
				std::string ret;
				if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
					return;
				} else if (e.type == SDL_KEYDOWN) {
					SDL_Scancode key = e.key.keysym.scancode;
					ret = fmt::format("Keyboard key={}({}), modifier={}.", int(key), key, SDL_Keymod(e.key.keysym.mod));
				} else if (e.type == SDL_JOYBUTTONDOWN) {
					ret = fmt::format("Joy ID={}, button={}, state={}.", int(e.jbutton.which), int(e.jbutton.button), int(e.jbutton.state));
				} else if (e.type == SDL_JOYAXISMOTION) {
					if ((oldjoy != int(e.jaxis.which)) || (oldaxis != int(e.jaxis.axis)) || (oldvalue != int(e.jaxis.value))) {
						ret = fmt::format("Joy ID={}, axis={}, value={}.", int(e.jaxis.which), int(e.jaxis.axis), int(e.jaxis.value));
						oldjoy = int(e.jaxis.which);
						oldaxis = int(e.jaxis.axis);
						oldvalue = int(e.jaxis.value);
					}
				} else if (e.type == SDL_JOYHATMOTION) {
					ret = fmt::format("Joy ID={}, hat={}, value={}.", int(e.jhat.which), int(e.jhat.hat), int(e.jhat.value));
				}
				if (!ret.empty()) {
					std::cout << ret << std::endl;	
				}
			}
			window.blank(); window.swap();
			std::this_thread::sleep_for(10ms); // Max 100 FPS
		}
	} catch (EXCEPTION& e) {
		SpdLogger::error(LogSystem::CONTROLLERS, "Caught error, exception={}", e.what());
	}
	return;
}

template <typename Container> void confOverride(Container const& c, std::string const& name) {
	if (c.empty()) return;  // Don't override if no options specified
	ConfigItem::StringList& sl = config[name].sl();
	sl.clear();
	std::copy(c.begin(), c.end(), std::back_inserter(sl));
}

void outputOptionalFeatureStatus();

static void fatalError(const std::string &msg) {
	auto errorMsg = fmt::format(
	"{}"
	"\nIf you think this is a bug in Performous, please report it at \n"
	"  https://github.com/performous/performous/issues"
	, msg);
	auto title = "FATAL ERROR";
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, errorMsg.c_str(), nullptr);
	SpdLogger::error(LogSystem::LOGGER, msg);
}

int main(int argc, char** argv) {
	Platform platform;
	std::srand(static_cast<unsigned>(std::time(nullptr)));
	// Parse commandline options
	std::vector<std::string> devices;
	std::vector<std::string> songdirs;
	namespace po = boost::program_options;
	po::options_description opt1("Generic options", 160, 80);
	std::string songlist;
	std::string logLevel;
	opt1.add_options()
	  ("help,h", "Print this message.")
	  ("?,?", "Print this message.")
	  ("log,l", po::value<std::string>(&logLevel)->value_name("<level>"), "Minimum level to log to console. Possible values are: trace, debug, info, notice, warn, error and off. Default is 'notice'. Unless 'trace' is specified, the logfile will always be set to 'debug'.")
	  ("version,v", "Display version number")
	  ("songlist", po::value<std::string>(&songlist)->value_name("<path>"), "Save a list of songs in the specified folder");
	po::options_description opt2("Configuration options", 160, 80);
	opt2.add_options()
	  ("audio", po::value<std::vector<std::string> >(&devices)->value_name("<device>")->composing(), "Specify a string to match audio devices to use; see audiohelp for details.")
	  ("audiohelp", "Print audio related information")
	  ("jstest", "Utility to get joystick button mappings");
	po::options_description opt3("Hidden options");
	opt3.add_options()
	  ("songdir", po::value<std::vector<std::string> >(&songdirs)->composing(), "");
	// Process flagless options as songdirs
	po::positional_options_description p;
	p.add("songdir", -1);
	po::options_description cmdline;
	cmdline.add(opt1).add(opt2);
	po::variables_map vm;
	// Load the arguments
	try {
		po::options_description allopts(cmdline);
		allopts.add(opt3);
		po::store(po::command_line_parser(argc, argv).options(allopts).positional(p).run(), vm);
	}	
	catch (boost::program_options::unknown_option const& e) {
		std::cerr << e.what() << '\n' << std::endl;
		std::cout << cmdline << "\n  Any arguments without a switch are interpreted as song folders.\n" << std::endl;
		return EXIT_SUCCESS;
	}
	catch (EXCEPTION& e) {
		std::cerr << "Error parsing program options. Exception=" << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	po::notify(vm);
	auto levelString = UnicodeUtil::toUpper(logLevel);
	auto levelEnum = spdlog::level::from_str(levelString);
	
	if (levelString != "OFF" && levelEnum == spdlog::level::off) {
		// spdlog::level::off is the default fallback for level::from_str, so only take that value into account if it was explicitly requested.
		levelEnum = spdlog::level::warn; // The default warn is our notice.
	}	
	if (vm.count("version")) {
		std::cout << PACKAGE " " VERSION << std::endl;
		return EXIT_SUCCESS;
	}
	if (vm.count("help") || vm.count("?")) {
		std::cout << cmdline << "\n  Any arguments without a switch are interpreted as song folders.\n" << std::endl;
		return EXIT_SUCCESS;
	}
	PathCache::pathBootstrap();
	SpdLogger spdLogger(levelEnum);
	try {
		outputOptionalFeatureStatus();

		readConfig();
		SpdLogger::toggleProfilerLogger();

		if (vm.count("audiohelp")) {
			SpdLogger::notice(LogSystem::LOGGER, "Starting the audio subsystem for audiohelp (errors printed on console may be ignored).");
			Audio audio;
			// Print the devices
			std::cout << portaudio::AudioBackends().dump();
			// Some examples
			std::cout << "Example --audio parameters" << std::endl;
			std::cout << "  --audio \"out=2\"         # Pick first working two-channel playback device" << std::endl;
			std::cout << "  --audio \"dev=1 out=2\"   # Pick device id 1 and assign stereo playback" << std::endl;
			std::cout << "  --audio 'dev=\"HDA Intel\" mics=blue,red'   # HDA Intel with two mics" << std::endl;
			std::cout << "  --audio 'dev=pulse out=2 mics=blue'       # PulseAudio with input and output" << std::endl;
			return EXIT_SUCCESS;
		}
		// Override XML config for options that were specified from commandline or performous.conf
		confOverride(songdirs, "paths/songs");
		confOverride(devices, "audio/devices");
		PathCache::getPaths(); // Initialize paths before other threads start
		if (vm.count("jstest")) { // Joystick test program
			SpdLogger::info(LogSystem::CONTROLLERS, 
				"Starting jstest input test utility.\n"
				"Joystick utility - Touch your joystick to see buttons here\n"
				"Hit ESC (window focused) to quit"
			);
			jstestLoop();
			SpdLogger::info(LogSystem::LOGGER, "Exiting normally.");
			return EXIT_SUCCESS;
		}
		// Run the game init and main loop
		mainLoop(songlist);
		SpdLogger::info(LogSystem::LOGGER, "Exiting normally.");
	}
	catch (EXCEPTION& e) {
		fatalError(e.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS; // Do not remove. SDL_Main (which this function is called on some platforms) needs return statement.
}

void outputOptionalFeatureStatus() {
	SpdLogger::notice(LogSystem::LOGGER,
		fmt::runtime(
			"{0} {1} starting...\n"
			"{5}Internationalization:     Enabled.\n"
			"{5}MIDI Hardware I/O:        {2}.\n"
			"{5}Webcam support:           {3}.\n"
			"{5}Webserver support:        {4}.\n"
		),
		PACKAGE,
		VERSION,
		input::Hardware::midiEnabled() ? "Enabled" : "Disabled",
		Webcam::enabled() ? "Enabled" : "Disabled",
		WebServer::enabled() ? "Enabled" : "Disabled",
		SpdLogger::newLineDec
	);
}
