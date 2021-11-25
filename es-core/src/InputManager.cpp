#include "InputManager.h"

#include "utils/FileSystemUtil.h"
#include "CECInput.h"
#include "Log.h"
#include "platform.h"
#include "Scripting.h"
#include "Window.h"
#include <pugixml/src/pugixml.hpp>
#include <SDL.h>
#include <iostream>
#include <assert.h>
#include <go2/input.h>

#define KEYBOARD_GUID_STRING "-1"
#define CEC_GUID_STRING      "-2"
#define GO2_GUID_STRING      "-3"

// SO HEY POTENTIAL POOR SAP WHO IS TRYING TO MAKE SENSE OF ALL THIS (by which I mean my future self)
// There are like four distinct IDs used for joysticks (crazy, right?)
// 1. Device index - this is the "lowest level" identifier, and is just the Nth joystick plugged in to the system (like /dev/js#).
//    It can change even if the device is the same, and is only used to open joysticks (required to receive SDL events).
// 2. SDL_JoystickID - this is an ID for each joystick that is supposed to remain consistent between plugging and unplugging.
//    ES doesn't care if it does, though.
// 3. "Device ID" - this is something I made up and is what InputConfig's getDeviceID() returns.
//    This is actually just an SDL_JoystickID (also called instance ID), but -1 means "keyboard" instead of "error."
// 4. Joystick GUID - this is some squashed version of joystick vendor, version, and a bunch of other device-specific things.
//    It should remain the same across runs of the program/system restarts/device reordering and is what I use to identify which joystick to load.

// hack for libgo2 input support
static go2_input_state_t* gamepadState;
static go2_input_state_t* prevGamepadState;
static go2_input_t* input;

// hack for cec support
int SDL_USER_CECBUTTONDOWN = -1;
int SDL_USER_CECBUTTONUP   = -1;

InputManager* InputManager::mInstance = NULL;

InputManager::InputManager() : mKeyboardInputConfig(NULL)
{
}

InputManager::~InputManager()
{
	deinit();
}

InputManager* InputManager::getInstance()
{
	if(!mInstance)
		mInstance = new InputManager();

	return mInstance;
}

void InputManager::init()
{
	if(initialized())
		deinit();

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
		Settings::getInstance()->getBool("BackgroundJoystickInput") ? "1" : "0");
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	SDL_JoystickEventState(SDL_ENABLE);

	// first, open all currently present joysticks
	int numJoysticks = SDL_NumJoysticks();
	for(int i = 0; i < numJoysticks; i++)
	{
		addJoystickByDeviceIndex(i);
	}

	mKeyboardInputConfig = new InputConfig(DEVICE_KEYBOARD, "Keyboard", KEYBOARD_GUID_STRING);
	loadInputConfig(mKeyboardInputConfig);

	SDL_USER_CECBUTTONDOWN = SDL_RegisterEvents(2);
	SDL_USER_CECBUTTONUP   = SDL_USER_CECBUTTONDOWN + 1;
	CECInput::init();
	mCECInputConfig = new InputConfig(DEVICE_CEC, "CEC", CEC_GUID_STRING);
	loadInputConfig(mCECInputConfig);


	input = go2_input_create();
	gamepadState = go2_input_state_create();
	prevGamepadState = go2_input_state_create();
	mGo2InputConfig = new InputConfig(DEVICE_GO2, "GO2", GO2_GUID_STRING);

	go2_input_state_read(input, gamepadState);
	

	mGo2InputConfig->mapInput("up", Input(DEVICE_GO2, TYPE_BUTTON, 0, 1, true));
	mGo2InputConfig->mapInput("down", Input(DEVICE_GO2, TYPE_BUTTON, 1, 1, true));
	mGo2InputConfig->mapInput("left", Input(DEVICE_GO2, TYPE_BUTTON, 2, 1, true));
	mGo2InputConfig->mapInput("right", Input(DEVICE_GO2, TYPE_BUTTON, 3, 1, true));

	mGo2InputConfig->mapInput("a", Input(DEVICE_GO2, TYPE_BUTTON, 4, 1, true));
	mGo2InputConfig->mapInput("b", Input(DEVICE_GO2, TYPE_BUTTON, 5, 1, true));
	mGo2InputConfig->mapInput("x", Input(DEVICE_GO2, TYPE_BUTTON, 6, 1, true));
	mGo2InputConfig->mapInput("y", Input(DEVICE_GO2, TYPE_BUTTON, 7, 1, true));

	mGo2InputConfig->mapInput("select", Input(DEVICE_GO2, TYPE_BUTTON, 8, 1, true));
	mGo2InputConfig->mapInput("start", Input(DEVICE_GO2, TYPE_BUTTON, 9, 1, true));

	mGo2InputConfig->mapInput("pageup", Input(DEVICE_GO2, TYPE_BUTTON, 10, 1, true));
	mGo2InputConfig->mapInput("pagedown", Input(DEVICE_GO2, TYPE_BUTTON, 11, 1, true));
	
	mGo2InputConfig->mapInput("prtscn", Input(DEVICE_GO2, TYPE_BUTTON, 12, 1, true));
}

void InputManager::addJoystickByDeviceIndex(int id)
{
	assert(id >= 0 && id < SDL_NumJoysticks());

	// open joystick & add to our list
	SDL_Joystick* joy = SDL_JoystickOpen(id);
	assert(joy);

	// add it to our list so we can close it again later
	SDL_JoystickID joyId = SDL_JoystickInstanceID(joy);
	mJoysticks[joyId] = joy;

	char guid[65];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, 65);

	// create the InputConfig
	mInputConfigs[joyId] = new InputConfig(joyId, SDL_JoystickName(joy), guid);
	if(!loadInputConfig(mInputConfigs[joyId]))
	{
		LOG(LogInfo) << "Added unconfigured joystick " << SDL_JoystickName(joy) << " (GUID: " << guid << ", instance ID: " << joyId << ", device index: " << id << ").";
	}else{
		LOG(LogInfo) << "Added known joystick " << SDL_JoystickName(joy) << " (instance ID: " << joyId << ", device index: " << id << ")";
	}

	// set up the prevAxisValues
	int numAxes = SDL_JoystickNumAxes(joy);
	mPrevAxisValues[joyId] = new int[numAxes];
	std::fill(mPrevAxisValues[joyId], mPrevAxisValues[joyId] + numAxes, 0); //initialize array to 0
}

void InputManager::removeJoystickByJoystickID(SDL_JoystickID joyId)
{
	assert(joyId != -1);

	// delete old prevAxisValues
	auto axisIt = mPrevAxisValues.find(joyId);
	delete[] axisIt->second;
	mPrevAxisValues.erase(axisIt);

	// delete old InputConfig
	auto it = mInputConfigs.find(joyId);
	delete it->second;
	mInputConfigs.erase(it);

	// close the joystick
	auto joyIt = mJoysticks.find(joyId);
	if(joyIt != mJoysticks.cend())
	{
		SDL_JoystickClose(joyIt->second);
		mJoysticks.erase(joyIt);
	}else{
		LOG(LogError) << "Could not find joystick to close (instance ID: " << joyId << ")";
	}
}

void InputManager::deinit()
{
	if(!initialized())
		return;

	for(auto iter = mJoysticks.cbegin(); iter != mJoysticks.cend(); iter++)
	{
		SDL_JoystickClose(iter->second);
	}
	mJoysticks.clear();

	for(auto iter = mInputConfigs.cbegin(); iter != mInputConfigs.cend(); iter++)
	{
		delete iter->second;
	}
	mInputConfigs.clear();

	for(auto iter = mPrevAxisValues.cbegin(); iter != mPrevAxisValues.cend(); iter++)
	{
		delete[] iter->second;
	}
	mPrevAxisValues.clear();

	if(mKeyboardInputConfig != NULL)
	{
		delete mKeyboardInputConfig;
		mKeyboardInputConfig = NULL;
	}

	if(mCECInputConfig != NULL)
	{
		delete mCECInputConfig;
		mCECInputConfig = NULL;
	}

	CECInput::deinit();

	SDL_JoystickEventState(SDL_DISABLE);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);


	go2_input_destroy(input);
	input = NULL;
	go2_input_state_destroy(gamepadState);
	gamepadState = NULL;
	go2_input_state_destroy(prevGamepadState);
	prevGamepadState = NULL;

	delete mGo2InputConfig;
	mGo2InputConfig = NULL;
}

int InputManager::getNumJoysticks() { return (int)mJoysticks.size(); }

int InputManager::getAxisCountByDevice(SDL_JoystickID id)
{
	return SDL_JoystickNumAxes(mJoysticks[id]);
}

int InputManager::getButtonCountByDevice(SDL_JoystickID id)
{
	if(id == DEVICE_KEYBOARD)
		return 120; //it's a lot, okay.
	else if(id == DEVICE_CEC)
#ifdef HAVE_CECLIB
		return CEC::CEC_USER_CONTROL_CODE_MAX;
#else // HAVE_LIBCEF
		return 0;
#endif // HAVE_CECLIB
	else
		return SDL_JoystickNumButtons(mJoysticks[id]);
}

InputConfig* InputManager::getInputConfigByDevice(int device)
{
	if(device == DEVICE_KEYBOARD)
		return mKeyboardInputConfig;
	else if(device == DEVICE_CEC)
		return mCECInputConfig;
	else
		return mInputConfigs[device];
}

bool InputManager::parseEvent(const SDL_Event& ev, Window* window)
{
	bool causedEvent = false;
	switch(ev.type)
	{
	case SDL_JOYAXISMOTION:
		//if it switched boundaries
		if((abs(ev.jaxis.value) > DEADZONE) != (abs(mPrevAxisValues[ev.jaxis.which][ev.jaxis.axis]) > DEADZONE))
		{
			int normValue;
			if(abs(ev.jaxis.value) <= DEADZONE)
				normValue = 0;
			else
				if(ev.jaxis.value > 0)
					normValue = 1;
				else
					normValue = -1;

			window->input(getInputConfigByDevice(ev.jaxis.which), Input(ev.jaxis.which, TYPE_AXIS, ev.jaxis.axis, normValue, false));
			causedEvent = true;
		}

		mPrevAxisValues[ev.jaxis.which][ev.jaxis.axis] = ev.jaxis.value;
		return causedEvent;

	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
		window->input(getInputConfigByDevice(ev.jbutton.which), Input(ev.jbutton.which, TYPE_BUTTON, ev.jbutton.button, ev.jbutton.state == SDL_PRESSED, false));
		return true;

	case SDL_JOYHATMOTION:
		window->input(getInputConfigByDevice(ev.jhat.which), Input(ev.jhat.which, TYPE_HAT, ev.jhat.hat, ev.jhat.value, false));
		return true;

	case SDL_KEYDOWN:
		if(ev.key.keysym.sym == SDLK_BACKSPACE && SDL_IsTextInputActive())
		{
			window->textInput("\b");
		}

		if(ev.key.repeat)
			return false;

		if(ev.key.keysym.sym == SDLK_F4)
		{
			SDL_Event* quit = new SDL_Event();
			quit->type = SDL_QUIT;
			SDL_PushEvent(quit);
			return false;
		}

		window->input(getInputConfigByDevice(DEVICE_KEYBOARD), Input(DEVICE_KEYBOARD, TYPE_KEY, ev.key.keysym.sym, 1, false));
		return true;

	case SDL_KEYUP:
		window->input(getInputConfigByDevice(DEVICE_KEYBOARD), Input(DEVICE_KEYBOARD, TYPE_KEY, ev.key.keysym.sym, 0, false));
		return true;

	case SDL_TEXTINPUT:
		window->textInput(ev.text.text);
		break;

	case SDL_JOYDEVICEADDED:
		addJoystickByDeviceIndex(ev.jdevice.which); // ev.jdevice.which is a device index
		return true;

	case SDL_JOYDEVICEREMOVED:
		removeJoystickByJoystickID(ev.jdevice.which); // ev.jdevice.which is an SDL_JoystickID (instance ID)
		return false;
	}

	if((ev.type == (unsigned int)SDL_USER_CECBUTTONDOWN) || (ev.type == (unsigned int)SDL_USER_CECBUTTONUP))
	{
		window->input(getInputConfigByDevice(DEVICE_CEC), Input(DEVICE_CEC, TYPE_CEC_BUTTON, ev.user.code, ev.type == (unsigned int)SDL_USER_CECBUTTONDOWN, false));
		return true;
	}

	return false;
}

void InputManager::processInput(Window* window)
{
	go2_input_state_t* tempState = prevGamepadState;
	prevGamepadState = gamepadState;
	gamepadState = tempState;

	go2_input_state_read(input, gamepadState);

	// DPad
	if (go2_input_state_button_get(gamepadState, Go2InputButton_DPadUp) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_DPadUp))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 0, go2_input_state_button_get(gamepadState, Go2InputButton_DPadUp) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_DPadDown) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_DPadDown))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 1, go2_input_state_button_get(gamepadState, Go2InputButton_DPadDown) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_DPadLeft) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_DPadLeft))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 2, go2_input_state_button_get(gamepadState, Go2InputButton_DPadLeft) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_DPadRight) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_DPadRight))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 3, go2_input_state_button_get(gamepadState, Go2InputButton_DPadRight) == ButtonState_Pressed, false));
	}

	// A/B/X/Y
	if (go2_input_state_button_get(gamepadState, Go2InputButton_A) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_A))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 4, go2_input_state_button_get(gamepadState, Go2InputButton_A) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_B) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_B))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 5, go2_input_state_button_get(gamepadState, Go2InputButton_B) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_X) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_X))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 6, go2_input_state_button_get(gamepadState, Go2InputButton_X) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_Y) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_Y))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 7, go2_input_state_button_get(gamepadState, Go2InputButton_Y) == ButtonState_Pressed, false));
	}

	// Select/Start
	if (go2_input_state_button_get(gamepadState, Go2InputButton_F3) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_F3))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 8, go2_input_state_button_get(gamepadState, Go2InputButton_F3) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_F4) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_F4))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 9, go2_input_state_button_get(gamepadState, Go2InputButton_F4) == ButtonState_Pressed, false));
	}

	// PageUp/PageDown
	if (go2_input_state_button_get(gamepadState, Go2InputButton_TopLeft) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_TopLeft))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 10, go2_input_state_button_get(gamepadState, Go2InputButton_TopLeft) == ButtonState_Pressed, false));
	}
	if (go2_input_state_button_get(gamepadState, Go2InputButton_TopRight) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_TopRight))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 11, go2_input_state_button_get(gamepadState, Go2InputButton_TopRight) == ButtonState_Pressed, false));
	}

	// PrintScreen
	if (go2_input_state_button_get(gamepadState, Go2InputButton_F2) !=
		go2_input_state_button_get(prevGamepadState, Go2InputButton_F2))
	{
		window->input(mGo2InputConfig, Input(DEVICE_GO2, TYPE_BUTTON, 12, go2_input_state_button_get(gamepadState, Go2InputButton_F2) == ButtonState_Pressed, false));
	}	
}

bool InputManager::loadInputConfig(InputConfig* config)
{
	std::string path = getConfigPath();
	if(!Utils::FileSystem::exists(path))
		return false;

	pugi::xml_document doc;
	pugi::xml_parse_result res = doc.load_file(path.c_str());

	if(!res)
	{
		LOG(LogError) << "Error parsing input config: " << res.description();
		return false;
	}

	pugi::xml_node root = doc.child("inputList");
	if(!root)
		return false;

	pugi::xml_node configNode = root.find_child_by_attribute("inputConfig", "deviceGUID", config->getDeviceGUIDString().c_str());
	if(!configNode)
		configNode = root.find_child_by_attribute("inputConfig", "deviceName", config->getDeviceName().c_str());
	if(!configNode)
		return false;

	config->loadFromXML(configNode);
	return true;
}

//used in an "emergency" where no keyboard config could be loaded from the inputmanager config file
//allows the user to select to reconfigure in menus if this happens without having to delete es_input.cfg manually
void InputManager::loadDefaultKBConfig()
{
	InputConfig* cfg = getInputConfigByDevice(DEVICE_KEYBOARD);

	cfg->clear();
	cfg->mapInput("up", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_UP, 1, true));
	cfg->mapInput("down", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_DOWN, 1, true));
	cfg->mapInput("left", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_LEFT, 1, true));
	cfg->mapInput("right", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RIGHT, 1, true));

	cfg->mapInput("a", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RETURN, 1, true));
	cfg->mapInput("b", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_ESCAPE, 1, true));
	cfg->mapInput("start", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_F1, 1, true));
	cfg->mapInput("select", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_F2, 1, true));

	cfg->mapInput("pageup", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RIGHTBRACKET, 1, true));
	cfg->mapInput("pagedown", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_LEFTBRACKET, 1, true));
}

void InputManager::writeDeviceConfig(InputConfig* config)
{
	assert(initialized());

	std::string path = getConfigPath();

	pugi::xml_document doc;

	if(Utils::FileSystem::exists(path))
	{
		// merge files
		pugi::xml_parse_result result = doc.load_file(path.c_str());
		if(!result)
		{
			LOG(LogError) << "Error parsing input config: " << result.description();
		}
		else
		{
			// successfully loaded, delete the old entry if it exists
			pugi::xml_node root = doc.child("inputList");
			if(root)
			{
				// if inputAction @type=onfinish is set, let onfinish command take care for creating input configuration.
				// we just put the input configuration into a temporary input config file.
				pugi::xml_node actionnode = root.find_child_by_attribute("inputAction", "type", "onfinish");
				if(actionnode)
				{
					path = getTemporaryConfigPath();
					doc.reset();
					root = doc.append_child("inputList");
					root.append_copy(actionnode);
				}
				else
				{
					pugi::xml_node oldEntry = root.find_child_by_attribute("inputConfig", "deviceGUID",
											  config->getDeviceGUIDString().c_str());
					if(oldEntry)
					{
						root.remove_child(oldEntry);
					}
					oldEntry = root.find_child_by_attribute("inputConfig", "deviceName",
															config->getDeviceName().c_str());
					if(oldEntry)
					{
						root.remove_child(oldEntry);
					}
				}
			}
		}
	}

	pugi::xml_node root = doc.child("inputList");
	if(!root)
		root = doc.append_child("inputList");

	config->writeToXML(root);
	doc.save_file(path.c_str());

	Scripting::fireEvent("config-changed");
	Scripting::fireEvent("controls-changed");

	// execute any onFinish commands and re-load the config for changes
	doOnFinish();
	loadInputConfig(config);
}

void InputManager::doOnFinish()
{
	assert(initialized());
	std::string path = getConfigPath();
	pugi::xml_document doc;

	if(Utils::FileSystem::exists(path))
	{
		pugi::xml_parse_result result = doc.load_file(path.c_str());
		if(!result)
		{
			LOG(LogError) << "Error parsing input config: " << result.description();
		}
		else
		{
			pugi::xml_node root = doc.child("inputList");
			if(root)
			{
				root = root.find_child_by_attribute("inputAction", "type", "onfinish");
				if(root)
				{
					for(pugi::xml_node command = root.child("command"); command;
							command = command.next_sibling("command"))
					{
						std::string tocall = command.text().get();

						LOG(LogInfo) << "	" << tocall;
						std::cout << "==============================================\ninput config finish command:\n";
						int exitCode = runSystemCommand(tocall);
						std::cout << "==============================================\n";

						if(exitCode != 0)
						{
							LOG(LogWarning) << "...launch terminated with nonzero exit code " << exitCode << "!";
						}
					}
				}
			}
		}
	}
}

std::string InputManager::getConfigPath()
{
	std::string path; // = Utils::FileSystem::getHomePath();
	//path += "/.emulationstation/es_input.cfg";
	//path += "/etc/emulationstation/es_input.cfg";
	path = "es_input.cfg";
	return path;
}

std::string InputManager::getTemporaryConfigPath()
{
	//std::string path = Utils::FileSystem::getHomePath();
	std::string path = "";
	path += "es_temporaryinput.cfg";
	return path;
}

bool InputManager::initialized() const
{
	return mKeyboardInputConfig != NULL;
}

int InputManager::getNumConfiguredDevices()
{
	int num = 0;
	for(auto it = mInputConfigs.cbegin(); it != mInputConfigs.cend(); it++)
	{
		if(it->second->isConfigured())
			num++;
	}

	if(mKeyboardInputConfig->isConfigured())
		num++;

	if(mCECInputConfig->isConfigured())
		num++;

	return num;
}

std::string InputManager::getDeviceGUIDString(int deviceId)
{
	if(deviceId == DEVICE_KEYBOARD)
		return KEYBOARD_GUID_STRING;

	if(deviceId == DEVICE_CEC)
		return CEC_GUID_STRING;

	auto it = mJoysticks.find(deviceId);
	if(it == mJoysticks.cend())
	{
		LOG(LogError) << "getDeviceGUIDString - deviceId " << deviceId << " not found!";
		return "something went horribly wrong";
	}

	char guid[65];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(it->second), guid, 65);
	return std::string(guid);
}
