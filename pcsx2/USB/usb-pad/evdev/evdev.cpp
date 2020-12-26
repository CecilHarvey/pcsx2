/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "evdev.h"
#include <cassert>
#include <sstream>
#include <linux/hidraw.h>
#include "../../linux/util.h"

namespace usb_pad
{
	namespace evdev
	{

		// hidraw* to input/event*:
		// /sys/class/hidraw/hidraw*/device/input/input*/event*/uevent

#define NORM(x, n) (((uint32_t)(32768 + x) * n) / 0xFFFF)
#define NORM2(x, n) (((uint32_t)(32768 + x) * n) / 0x7FFF)

		bool str_ends_with(const char* str, const char* suffix)
		{
			if (str == nullptr || suffix == nullptr)
				return false;

			size_t str_len = strlen(str);
			size_t suffix_len = strlen(suffix);

			if (suffix_len > str_len)
				return false;

			return 0 == strncmp(str + str_len - suffix_len, suffix, suffix_len);
		}

		bool FindHidraw(const std::string& evphys, std::string& hid_dev, int* vid, int* pid)
		{
			int fd;
			int res;
			char buf[256];

			std::stringstream str;
			struct dirent* dp;

			DIR* dirp = opendir("/dev/");
			if (!dirp)
			{
				Console.Warning("Error opening /dev/");
				return false;
			}

			while ((dp = readdir(dirp)))
			{
				if (strncmp(dp->d_name, "hidraw", 6) == 0)
				{

					str.clear();
					str.str("");
					str << "/dev/" << dp->d_name;
					std::string path = str.str();
					fd = open(path.c_str(), O_RDWR | O_NONBLOCK);

					if (fd < 0)
					{
						Console.Warning("Evdev: Unable to open device: %s", path.c_str());
						continue;
					}

					memset(buf, 0x0, sizeof(buf));
					//res = ioctl(fd, HIDIOCGRAWNAME(256), buf);

					res = ioctl(fd, HIDIOCGRAWPHYS(sizeof(buf)), buf);
					if (res < 0)
						Console.Warning("HIDIOCGRAWPHYS");

					struct hidraw_devinfo info;
					memset(&info, 0x0, sizeof(info));

					if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0)
					{
						Console.Warning("HIDIOCGRAWINFO");
					}
					else
					{
						if (vid)
							*vid = info.vendor;
						if (pid)
							*pid = info.product;
					}

					close(fd);
					if (evphys == buf)
					{
						closedir(dirp);
						hid_dev = path;
						return true;
					}
				}
			}
		//quit:
			closedir(dirp);
			return false;
		}

#define EVDEV_DIR "/dev/input/by-id/"
		void EnumerateDevices(device_list& list)
		{
			int fd;
			int res;
			char buf[256];

			std::stringstream str;
			struct dirent* dp;

			//TODO do some caching? ioctl is "very" slow
			static device_list list_cache;

			DIR* dirp = opendir(EVDEV_DIR);
			if (!dirp)
			{
				Console.Warning("Error opening " EVDEV_DIR);
				return;
			}

			// get rid of unplugged devices
			for (int i = 0; i < (int)list_cache.size();)
			{
				if (!file_exists(list_cache[i].path))
					list_cache.erase(list_cache.begin() + i);
				else
					i++;
			}

			while ((dp = readdir(dirp)))
			{
				//if (strncmp(dp->d_name, "event", 5) == 0) {
				if (str_ends_with(dp->d_name, "event-kbd") || str_ends_with(dp->d_name, "event-mouse") || str_ends_with(dp->d_name, "event-joystick"))
				{

					str.clear();
					str.str("");
					str << EVDEV_DIR << dp->d_name;
					std::string path = str.str();

					auto it = std::find_if(list_cache.begin(), list_cache.end(),
										   [&path](evdev_device& dev) {
											   return dev.path == path;
										   });
					if (it != list_cache.end())
						continue;

					fd = open(path.c_str(), O_RDWR | O_NONBLOCK);

					if (fd < 0)
					{
						Console.Warning("Evdev: Unable to open device: %s", path.c_str());
						continue;
					}

					//list_cache.push_back(std::make_pair(std::string(dp->d_name), path));

					res = ioctl(fd, EVIOCGNAME(sizeof(buf)), buf);
					if (res < 0)
						Console.Warning("EVIOCGNAME");
					else
					{
						list_cache.push_back({buf, dp->d_name, path});
					}

					close(fd);
				}
			}

			list.assign(list_cache.begin(), list_cache.end());
		//quit:
			closedir(dirp);
		}

		void EvDevPad::PollAxesValues(const device_data& device)
		{
			struct input_absinfo absinfo;

			/* Poll all axis */
			for (int i = ABS_X; i < ABS_MAX; i++)
			{
				absinfo = {};

				if ((ioctl(device.cfg.fd, EVIOCGABS(i), &absinfo) >= 0) &&
					device.abs_correct[i].used)
				{
					absinfo.value = AxisCorrect(device.abs_correct[i], absinfo.value);
				}
				SetAxis(device, i, absinfo.value);
			}
		}

		void EvDevPad::SetAxis(const device_data& device, int event_code, int value)
		{
			int range = range_max(mType);
			int code = device.axis_map[event_code] != (uint8_t)-1 ? device.axis_map[event_code] : -1 /* allow axis to be unmapped */; //event_code;
			//value = AxisCorrect(mAbsCorrect[event_code], value);

			switch (code)
			{
				case 0x80 | JOY_STEERING:
				case ABS_X:
					mWheelData.steering = device.cfg.inverted[0] ? range - NORM(value, range) : NORM(value, range);
					break;
				//case ABS_Y: mWheelData.clutch = NORM(value, 0xFF); break; //no wheel on PS2 has one, afaik
				//case ABS_RX: mWheelData.axis_rx = NORM(event.value, 0xFF); break;
				case ABS_RY:
				//treat_me_like_ABS_RY:
					mWheelData.throttle = 0xFF;
					mWheelData.brake = 0xFF;
					if (value < 0)
						mWheelData.throttle = NORM2(value, 0xFF);
					else
						mWheelData.brake = NORM2(-value, 0xFF);
					break;
				case 0x80 | JOY_THROTTLE:
				case ABS_Z:
					/*if (mIsGamepad)
				mWheelData.brake = 0xFF - NORM(value, 0xFF);
			else*/
					mWheelData.throttle = device.cfg.inverted[1] ? NORM(value, 0xFF) : 0xFF - NORM(value, 0xFF);
					break;
				case 0x80 | JOY_BRAKE:
				case ABS_RZ:
					/*if (mIsGamepad)
				mWheelData.throttle = 0xFF - NORM(value, 0xFF);
			else if (mIsDualAnalog)
				goto treat_me_like_ABS_RY;
			else*/
					mWheelData.brake = device.cfg.inverted[2] ? NORM(value, 0xFF) : 0xFF - NORM(value, 0xFF);
					break;

				//TODO hatswitch mapping maybe
				case ABS_HAT0X:
				case ABS_HAT1X:
				case ABS_HAT2X:
				case ABS_HAT3X:
					if (value < 0) //left usually
						mWheelData.hat_horz = PAD_HAT_W;
					else if (value > 0) //right
						mWheelData.hat_horz = PAD_HAT_E;
					else
						mWheelData.hat_horz = PAD_HAT_COUNT;
					break;
				case ABS_HAT0Y:
				case ABS_HAT1Y:
				case ABS_HAT2Y:
				case ABS_HAT3Y:
					if (value < 0) //up usually
						mWheelData.hat_vert = PAD_HAT_N;
					else if (value > 0) //down
						mWheelData.hat_vert = PAD_HAT_S;
					else
						mWheelData.hat_vert = PAD_HAT_COUNT;
					break;
				default:
					break;
			}
		}

		int EvDevPad::TokenIn(uint8_t* buf, int buflen)
		{
			ssize_t len;

			input_event events[32];
			fd_set fds;
			int maxfd;

			FD_ZERO(&fds);
			maxfd = -1;

			for (auto& device : mDevices)
			{
				FD_SET(device.cfg.fd, &fds);
				if (maxfd < device.cfg.fd)
					maxfd = device.cfg.fd;
			}

			struct timeval timeout;
			timeout.tv_usec = timeout.tv_sec = 0; // 0 - return from select immediately
			int result = select(maxfd + 1, &fds, NULL, NULL, &timeout);

			if (result <= 0)
			{
				return USB_RET_NAK; // If no new data, NAK it
			}

			for (auto& device : mDevices)
			{
				if (!FD_ISSET(device.cfg.fd, &fds))
				{
					continue;
				}

				//Non-blocking read sets len to -1 and errno to EAGAIN if no new data
				while ((len = read(device.cfg.fd, &events, sizeof(events))) > -1)
				{
					len /= sizeof(events[0]);
					for (int i = 0; i < len; i++)
					{
						input_event& event = events[i];
						int code, value;
						switch (event.type)
						{
							case EV_ABS:
							{
								if (mType == WT_BUZZ_CONTROLLER)
									break;

								value = AxisCorrect(device.abs_correct[event.code], event.value);
								/*if (event.code == 0)
								event.code, device.axis_map[event.code] & ~0x80, event.value, value);
						*/
								SetAxis(device, event.code, value);
							}
							break;
							case EV_KEY:
							{
								code = device.btn_map[event.code] != (uint16_t)-1 ? device.btn_map[event.code] : event.code;

								if (mType == WT_BUZZ_CONTROLLER)
								{
									if (device.btn_map[event.code] != (uint16_t)-1)
									{
										if (event.value)
											mWheelData.buttons |= 1 << (code & ~0x8000); //on
										else
											mWheelData.buttons &= ~(1 << (code & ~0x8000)); //off
									}

									break;
								}

								PS2Buttons button = PAD_BUTTON_COUNT;
								if (code >= (0x8000 | JOY_CROSS) && // user mapped
									code <= (0x8000 | JOY_L3))
								{
									button = (PS2Buttons)(code & ~0x8000);
								}
								else if (code >= BTN_TRIGGER && code < BTN_BASE5) // try to guess
								{
									button = (PS2Buttons)((code - BTN_TRIGGER) & ~0x8000);
								}
								else
								{
#if 0
									// Map to xbox360ish controller
									switch (code)
									{
										// Digital hatswitch
										case 0x8000 | JOY_LEFT:
											mWheelData.hat_horz = (!event.value ? PAD_HAT_COUNT : PAD_HAT_W);
											break;
										case 0x8000 | JOY_RIGHT:
											mWheelData.hat_horz = (!event.value ? PAD_HAT_COUNT : PAD_HAT_E);
											break;
										case 0x8000 | JOY_UP:
											mWheelData.hat_vert = (!event.value ? PAD_HAT_COUNT : PAD_HAT_N);
											break;
										case 0x8000 | JOY_DOWN:
											mWheelData.hat_vert = (!event.value ? PAD_HAT_COUNT : PAD_HAT_S);
											break;
										case BTN_WEST:
											button = PAD_SQUARE;
											break;
										case BTN_NORTH:
											button = PAD_TRIANGLE;
											break;
										case BTN_EAST:
											button = PAD_CIRCLE;
											break;
										case BTN_SOUTH:
											button = PAD_CROSS;
											break;
										case BTN_SELECT:
											button = PAD_SELECT;
											break;
										case BTN_START:
											button = PAD_START;
											break;
										case BTN_TR:
											button = PAD_R1;
											break;
										case BTN_TL:
											button = PAD_L1;
											break;
										case BTN_TR2:
											button = PAD_R2;
											break;
										case BTN_TL2:
											button = PAD_L2;
											break;
										default:
											break;
									}
#endif
								}

								//if (button != PAD_BUTTON_COUNT)
								{
									if (event.value)
										mWheelData.buttons |= 1 << convert_wt_btn(mType, button); //on
									else
										mWheelData.buttons &= ~(1 << convert_wt_btn(mType, button)); //off
								}
							}
							break;
							case EV_SYN: //TODO useful?
							{
								switch (event.code)
								{
									case SYN_DROPPED:
										//restore last good state
										mWheelData = {};
										PollAxesValues(device);
										break;
								}
							}
							break;
							default:
								break;
						}
					}

					if (len <= 0)
					{
						break;
					}
				}
			}

			switch (mWheelData.hat_vert)
			{
				case PAD_HAT_N:
					switch (mWheelData.hat_horz)
					{
						case PAD_HAT_W:
							mWheelData.hatswitch = PAD_HAT_NW;
							break;
						case PAD_HAT_E:
							mWheelData.hatswitch = PAD_HAT_NE;
							break;
						default:
							mWheelData.hatswitch = PAD_HAT_N;
							break;
					}
					break;
				case PAD_HAT_S:
					switch (mWheelData.hat_horz)
					{
						case PAD_HAT_W:
							mWheelData.hatswitch = PAD_HAT_SW;
							break;
						case PAD_HAT_E:
							mWheelData.hatswitch = PAD_HAT_SE;
							break;
						default:
							mWheelData.hatswitch = PAD_HAT_S;
							break;
					}
					break;
				default:
					mWheelData.hatswitch = mWheelData.hat_horz;
					break;
			}

			pad_copy_data(mType, buf, mWheelData);
			return buflen;
		}

		int EvDevPad::TokenOut(const uint8_t* data, int len)
		{
			if (mUseRawFF)
			{

				if (data[0] == 0x8 || data[0] == 0xB)
					return len;
				if (data[0] == 0xF8 &&
					/* Allow range changes */
					!(data[1] == 0x81 || data[1] == 0x02 || data[1] == 0x03))
					return len; //don't send extended commands

				std::array<uint8_t, 8> report{0};

				memcpy(report.data() + 1, data, report.size() - 1);

				if (!mFFData.enqueue(report))
				{
					return 0;
				}
				return len;
			}

			const ff_data* ffdata = (const ff_data*)data;
			bool hires = (mType == WT_DRIVING_FORCE_PRO || mType == WT_DRIVING_FORCE_PRO_1102);
			ParseFFData(ffdata, hires);

			return len;
		}

		int EvDevPad::Open()
		{
			std::stringstream name;
			device_list device_list;
			char buf[1024];
			mWheelData = {};
			int32_t b_gain, gain, b_ac, ac;

			unsigned long keybit[NBITS(KEY_MAX)];
			unsigned long absbit[NBITS(ABS_MAX)];
			memset(keybit, 0, sizeof(keybit));
			memset(absbit, 0, sizeof(absbit));

			// Setting to unpressed
			mWheelData.steering = 0x3FF >> 1;
			mWheelData.clutch = 0xFF;
			mWheelData.throttle = 0xFF;
			mWheelData.brake = 0xFF;
			mWheelData.hatswitch = 0x8;
			mWheelData.hat_horz = 0x8;
			mWheelData.hat_vert = 0x8;
			//memset(mAxisMap, -1, sizeof(mAxisMap));
			//memset(mBtnMap, -1, sizeof(mBtnMap));

			//mAxisCount = 0;
			//mButtonCount = 0;
			//mHandle = -1;

			std::string evphys, hid_dev;

			switch (mType)
			{
				case WT_GENERIC:
				case WT_GT_FORCE:
				case WT_DRIVING_FORCE_PRO:
				case WT_DRIVING_FORCE_PRO_1102:
				{
					if (!LoadSetting(mDevType, mPort, APINAME, N_HIDRAW_FF_PT, mUseRawFF))
						mUseRawFF = 0;
				}
				break;
				default:
					break;
			}

			if (mUseRawFF)
			{
				// TODO could just use device fd below whose axis is mapped to steering
				std::string joypath;
				if (!LoadSetting(mDevType, mPort, APINAME, N_JOYSTICK, joypath))
				{
					return 1;
				}

				if (joypath.empty() || !file_exists(joypath))
					goto quit;

				int fd = -1;
				if ((fd = open(joypath.c_str(), O_RDWR | O_NONBLOCK)) < 0)
				{
					return 1;
				}

				memset(buf, 0, sizeof(buf));
				if (ioctl(fd, EVIOCGPHYS(sizeof(buf) - 1), buf) > 0)
				{
					evphys = buf;

					int pid, vid;
					if ((mUseRawFF = FindHidraw(evphys, hid_dev, &vid, &pid)))
					{

						// For safety, only allow Logitech (classic ffb) devices
						if (vid != 0x046D /* Logitech */ /*|| info.bustype != BUS_USB*/
							|| pid == 0xc262             /* G920 hid mode */
							|| pid == 0xc261             /* G920 xbox mode */
						)
						{
							mUseRawFF = 0;
						}

						// check if still using hidraw and run the thread
						if (mUseRawFF && !mWriterThreadIsRunning)
						{
							if (mWriterThread.joinable())
								mWriterThread.join();
							mWriterThread = std::thread(&EvDevPad::WriterThread, this);
						}
					}
				}
				else
				{
					Console.Warning("EVIOCGPHYS failed");
				}
				close(fd);
			}

			EnumerateDevices(device_list);

			for (const auto& it : device_list)
			{
				bool has_mappings = false;
				mDevices.push_back({});

				struct device_data& device = mDevices.back();
				device.name = it.name;

				if ((device.cfg.fd = open(it.path.c_str(), O_RDWR | O_NONBLOCK)) < 0)
				{
					continue;
				}

				int ret_abs = ioctl(device.cfg.fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
				int ret_key = ioctl(device.cfg.fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
				memset(device.axis_map, 0xFF, sizeof(device.axis_map));
				memset(device.btn_map, 0xFF, sizeof(device.btn_map));

				if ((ret_abs < 0) && (ret_key < 0))
				{
					// Probably isn't a evdev joystick
					SysMessage("%s: Getting atleast some of the bits failed: %s\n", APINAME, strerror(errno));
					continue;
				}

				/*unsigned int version;
		if (ioctl(mHandle, EVIOCGVERSION, &version) < 0)
		{
			SysMessage("%s: Get version failed: %s\n", APINAME, strerror(errno));
			return false;
		}*/

				int max_buttons = JOY_STEERING;
				switch (mType)
				{
					case WT_BUZZ_CONTROLLER:
						LoadBuzzMappings(mDevType, mPort, it.id, device.cfg);
						max_buttons = 20;
						break;
					default:
						LoadMappings(mDevType, mPort, it.id, device.cfg);
						if (!LoadSetting(mDevType, mPort, APINAME, N_GAIN_ENABLED, b_gain))
							b_gain = 1;
						if (!LoadSetting(mDevType, mPort, APINAME, N_GAIN, gain))
							gain = 100;
						if (!LoadSetting(mDevType, mPort, APINAME, N_AUTOCENTER_MANAGED, b_ac))
							b_ac = 1;
						if (!LoadSetting(mDevType, mPort, APINAME, N_AUTOCENTER, ac))
							ac = 100;
						break;
				}

				// Map hatswitches automatically
				//FIXME has_mappings is gonna ignore hatsw only devices
				for (int i = ABS_HAT0X; i <= ABS_HAT3Y; ++i)
				{
					device.axis_map[i] = i;
				}

				// SDL2
				for (int i = 0; i < ABS_MAX; ++i)
				{
					if (test_bit(i, absbit))
					{
						struct input_absinfo absinfo;

						if (ioctl(device.cfg.fd, EVIOCGABS(i), &absinfo) < 0)
						{
							continue;
						}


						//device.axis_map[i] = device.axes;

						// convert values into 16 bit range
						CalcAxisCorr(device.abs_correct[i], absinfo);

						//TODO joystick/gamepad is dual analog?
						if (i == ABS_RZ)
						{
							//absinfo.value = AxisCorrect(mAbsCorrect[i], absinfo.value);
							if (std::abs(absinfo.value) < 200) /* 200 is random, allows for some dead zone */
								device.is_dualanalog = true;
						}

						// FIXME axes as buttons
						for (int k = max_buttons /*JOY_STEERING*/; k < JOY_MAPS_COUNT; k++)
						{
							if (i == device.cfg.controls[k])
							{
								has_mappings = true;
								device.axis_map[i] = 0x80 | k;
								// TODO Instead of single FF instance, create for every device with X-axis???
								// and then switch between them according to which device was used recently
								if (k == JOY_STEERING && !mFFdev && !mUseRawFF)
								{
									mFFdev = new EvdevFF(device.cfg.fd, b_gain, gain, b_ac, ac);
								}
							}
						}
					}
				}

#ifndef NDEBUG
				for (int i = 0; i < ABS_MAX; ++i)
				{
				}
#endif

				for (int i = BTN_JOYSTICK; i < KEY_MAX; ++i)
				{
					if (test_bit(i, keybit))
					{
						device.btn_map[i] = -1; //device.buttons;
						if (i == BTN_GAMEPAD)
						{
							device.is_gamepad = true;
						}
						for (int k = 0; k < max_buttons; k++)
						{
							if (i == device.cfg.controls[k])
							{
								has_mappings = true;
								device.btn_map[i] = 0x8000 | k;
							}
						}
					}
				}
				for (int i = 0; i < BTN_JOYSTICK; ++i)
				{
					if (test_bit(i, keybit))
					{
						device.btn_map[i] = -1; //device.buttons;
						for (int k = 0; k < max_buttons; k++)
						{
							if (i == device.cfg.controls[k])
							{
								has_mappings = true;
								device.btn_map[i] = 0x8000 | k;
							}
						}
					}
				}
				if (!has_mappings)
				{
					close(device.cfg.fd);
					mDevices.pop_back();
				}
			}

			return 0;

		quit:
			Close();
			return 1;
		}

		int EvDevPad::Close()
		{
			delete mFFdev;
			mFFdev = nullptr;

			if (mHidHandle != -1)
			{
				uint8_t reset[7] = {0};
				reset[0] = 0xF3; //stop forces
				write(mHidHandle, reset, sizeof(reset));
				close(mHidHandle);
			}

			mHidHandle = -1;
			for (auto& it : mDevices)
			{
				close(it.cfg.fd);
				it.cfg.fd = -1;
			}
			mDevices.clear();
			return 0;
		}

		void EvDevPad::WriterThread()
		{
			std::array<uint8_t, 8> buf;
			int res;

			mWriterThreadIsRunning = true;

			while (mHidHandle != -1)
			{
				//if (mFFData.wait_dequeue_timed(buf, std::chrono::milliseconds(1000))) //FIXME SIGABORT :S
				if (mFFData.try_dequeue(buf))
				{
					res = write(mHidHandle, buf.data(), buf.size());
					if (res < 0)
					{
						Console.Warning("write");
					}
				}
				else
				{ // TODO skip sleep for few while cycles?
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}

			mWriterThreadIsRunning = false;
		}

	} // namespace evdev
} // namespace usb_pad
