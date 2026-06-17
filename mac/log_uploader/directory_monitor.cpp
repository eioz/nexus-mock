#include "directory_monitor.h"
#include "log_manager.h"
#include "addon.h"
#include "platform/platform.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>

IMPLEMENT_MODULE(DirectoryMonitor, directory_monitor)

void DirectoryMonitor::initialize()
{
	monitor_directory = addon::get_log_directory();

	if (monitor_directory.empty())
		monitor_directory = addon::platform::get_default_log_directory();

	std::error_code ec;
	std::filesystem::create_directories(monitor_directory, ec);

	initialized.store(true);

	addon::log("Monitoring directory: " + monitor_directory.string(), LOGLEVEL_INFO);
	monitor_thread = std::thread(&DirectoryMonitor::run, this);
}

void DirectoryMonitor::release()
{
	initialized.store(false);

	if (monitor_thread.joinable())
		monitor_thread.join();

	monitor_directory.clear();
}

void DirectoryMonitor::run()
{
	if (monitor_directory.empty())
		return;

	addon::log("Started mock directory polling: " + monitor_directory.string(), LOGLEVEL_DEBUG);

	std::unordered_set<std::string> seen;

	while (initialized.load())
	{
		std::error_code ec;
		if (std::filesystem::exists(monitor_directory, ec))
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(monitor_directory, std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (ec)
					break;

				if (!entry.is_regular_file(ec))
					continue;

				auto path = entry.path();
				auto extension = path.extension().string();
				if (extension != ".evtc" && extension != ".zevtc")
					continue;

				auto key = path.string();
				if (!seen.insert(key).second)
					continue;

				addon::log("Mock evtc file detected: " + key, LOGLEVEL_DEBUG);
				addon::log_manager->add_log(path);
			}
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	addon::log("Mock directory polling stopped", LOGLEVEL_DEBUG);
}
