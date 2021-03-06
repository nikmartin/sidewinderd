/*
 * Copyright (c) 2014 - 2015 Tolga Cakir <tolga@cevel.net>
 *
 * This source file is part of Sidewinder daemon and is distributed under the
 * MIT License. For more information, see LICENSE file.
 */

#include <cstdlib>
#include <csignal>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <getopt.h>
#include <libudev.h>
#include <pwd.h>
#include <unistd.h>

#include <libconfig.h++>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "device_data.hpp"
#include "keyboard.hpp"
#include "sidewinderd.hpp"
#include "virtual_input.hpp"

void sig_handler(int sig) {
	switch (sig) {
		case SIGINT:
			sidewinderd::state = 0;
			break;
		case SIGTERM:
			sidewinderd::state = 0;
			break;
		default:
			std::cout << "Unknown signal received." << std::endl;
	}
}

int create_pid(std::string pid_file) {
	int pid_fd = open(pid_file.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	if (pid_fd < 0) {
		std::cout << "PID file could not be created." << std::endl;
		return -1;
	}

	if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0) {
		std::cout << "Could not lock PID file, another instance is already running. Terminating." << std::endl;
		close(pid_fd);
		return -1;
	}

	return pid_fd;
}

void close_pid(int pid_fd, std::string pid_file) {
	flock(pid_fd, LOCK_UN);
	close(pid_fd);
	unlink(pid_file.c_str());
}

int daemonize() {
	pid_t pid, sid;

	pid = fork();

	if (pid < 0) {
		std::cout << "Error creating daemon" << std::endl;
		return -1;
	}

	if (pid > 0) {
		return 1;
	}

	sid = setsid();

	if (sid < 0) {
		std::cout << "Error setting sid" << std::endl;
		return -1;
	}

	pid = fork();

	if (pid < 0) {
		std::cout << "Error forking second time" << std::endl;
		return -1;
	}

	if (pid > 0) {
		return 1;
	}

	umask(0);
	chdir("/");

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	return 0;
}

void setup_config(libconfig::Config *config, std::string config_file = "/etc/sidewinderd.conf") {
	try {
		config->readFile(config_file.c_str());
	} catch (const libconfig::FileIOException &fioex) {
		std::cerr << "I/O error while reading file." << std::endl;
	} catch (const libconfig::ParseException &pex) {
		std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine() << " - " << pex.getError() << std::endl;
	}

	libconfig::Setting &root = config->getRoot();

	/* TODO: check values for validity and throw exceptions, if invalid */
	if (!root.exists("user")) {
		root.add("user", libconfig::Setting::TypeString) = "root";
	}

	if (!root.exists("profile")) {
		root.add("profile", libconfig::Setting::TypeInt) = 1;
	}

	if (!root.exists("capture_delays")) {
		root.add("capture_delays", libconfig::Setting::TypeBoolean) = true;
	}

	if (!root.exists("pid-file")) {
		root.add("pid-file", libconfig::Setting::TypeString) = "/var/run/sidewinderd.pid";
	}

	try {
		config->writeFile(config_file.c_str());
	} catch (const libconfig::FileIOException &fioex) {
		std::cerr << "I/O error while writing file." << std::endl;
	}
}

int search_device(struct sidewinderd::DeviceData *data) {
	struct udev *udev;
	struct udev_device *dev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	int found = 0;

	udev = udev_new();

	if (!udev) {
		std::cout << "Can't create udev" << std::endl;

		return -1;
	}

	enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "hidraw");
	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *syspath, *devnode_path;
		syspath = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(udev, syspath);

		if (std::string(udev_device_get_subsystem(dev)) == std::string("hidraw")) {
			devnode_path = udev_device_get_devnode(dev);
			dev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_interface");

			if (!dev) {
				std::cout << "Unable to find parent device" << std::endl;
			}

			if (std::string(udev_device_get_sysattr_value(dev, "bInterfaceNumber")) == std::string("01")) {
				dev = udev_device_get_parent_with_subsystem_devtype(dev, "usb", "usb_device");

				if (std::string(udev_device_get_sysattr_value(dev, "idVendor")) == data->vid) {
					if (std::string(udev_device_get_sysattr_value(dev, "idProduct")) == data->pid) {
						std::cout << "Found device: " << data->vid << ":" << data->pid << std::endl;
						found = 1;
						data->devnode.hidraw = devnode_path;
					}
				}
			}
		}

		/* find correct /dev/input/event* file */
		if (std::string(udev_device_get_subsystem(dev)) == std::string("input")
			&& udev_device_get_property_value(dev, "ID_MODEL_ID") != NULL
			&& std::string(udev_device_get_property_value(dev, "ID_MODEL_ID")) == data->pid
			&& udev_device_get_property_value(dev, "ID_VENDOR_ID") != NULL
			&& std::string(udev_device_get_property_value(dev, "ID_VENDOR_ID")) == data->vid
			&& udev_device_get_property_value(dev, "ID_INPUT_KEYBOARD") != NULL
			&& strstr(syspath, "event")
			&& udev_device_get_parent_with_subsystem_devtype(dev, "usb", NULL)) {
				data->devnode.input_event = udev_device_get_devnode(dev);
		}

		udev_device_unref(dev);
	}

	/* free the enumerator object */
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	return found;
}

int main(int argc, char *argv[]) {
	/* signal handling */
	struct sigaction action;

	action.sa_handler = sig_handler;

	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);

	sidewinderd::state = 1;

	/* handling command-line options */
	static struct option long_options[] = {
		{"config", required_argument, 0, 'c'},
		{"daemon", no_argument, 0, 'd'},
		{"verbose", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};

	int opt, index, ret;
	index = 0;

	std::string config_file;

	while ((opt = getopt_long(argc, argv, ":c:dv", long_options, &index)) != -1) {
		switch (opt) {
			case 'c':
				config_file = optarg;
				break;
			case 'd':
				ret = daemonize();

				if (ret > 0) {
					return EXIT_SUCCESS;
				}

				if (ret < 0) {
					return EXIT_FAILURE;
				};

				break;
			case 'v':
				std::cout << "Option --verbose" << std::endl;
				break;
			case ':':
				std::cout << "Missing argument." << std::endl;
				break;
			case '?':
				std::cout << "Unrecognized option." << std::endl;
				break;
			default:
				std::cout << "Unexpected error." << std::endl;
				return EXIT_FAILURE;
		}
	}

	/* reading config file */
	libconfig::Config config;

	if (config_file.empty()) {
		setup_config(&config);
	} else {
		setup_config(&config, config_file);
	}

	/* creating pid file for single instance mechanism */
	std::string pid_file = config.lookup("pid-file");
	int pid_fd = create_pid(pid_file);

	if (pid_fd < 0) {
		return EXIT_FAILURE;
	}

	/* get user's home directory */
	std::string user = config.lookup("user");
	struct passwd *pw = getpwnam(user.c_str());

	/* setting gid and uid to configured user */
	setegid(pw->pw_gid);
	seteuid(pw->pw_uid);

	/* creating sidewinderd directory in user's home directory */
	std::string workdir = pw->pw_dir;
	workdir.append("/.sidewinderd");
	mkdir(workdir.c_str(), S_IRWXU);

	if (chdir(workdir.c_str())) {
		std::cout << "Error chdir" << std::endl;
	}

	std::cout << "Sidewinderd v" << sidewinderd::version << " has been started." << std::endl;

	for (std::vector<std::pair<std::string, std::string>>::iterator it = sidewinderd::devices.begin(); it != sidewinderd::devices.end(); ++it) {
		struct sidewinderd::DeviceData data;
		data.vid = it->first;
		data.pid = it->second;

		if (search_device(&data) > 0) {
			Keyboard kbd(&data, &config, pw);

			/* main loop */
			/* TODO: exit loop, if keyboards gets unplugged */
			while (sidewinderd::state) {
				kbd.listen();
			}
		}
	}

	close_pid(pid_fd, pid_file);

	std::cout << "Sidewinderd has been terminated." << std::endl;

	return EXIT_SUCCESS;
}
