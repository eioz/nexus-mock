#include "platform/platform.h"

#import <AppKit/AppKit.h>

#include <cctype>
#include <fstream>

#ifndef NEXUS_MOCK_DATA_DIR
#define NEXUS_MOCK_DATA_DIR "."
#endif

namespace addon::platform
{
void open_url(const std::string& url)
{
	if (url.rfind("mock://", 0) == 0)
		return;

	@autoreleasepool
	{
		NSString* value = [NSString stringWithUTF8String:url.c_str()];
		NSURL* ns_url = [NSURL URLWithString:value];
		if (ns_url != nil)
			[[NSWorkspace sharedWorkspace] openURL:ns_url];
	}
}

void open_file(const std::filesystem::path& path)
{
	@autoreleasepool
	{
		NSString* value = [NSString stringWithUTF8String:path.string().c_str()];
		NSURL* url = [NSURL fileURLWithPath:value];
		if (url != nil)
			[[NSWorkspace sharedWorkspace] openURL:url];
	}
}

bool copy_text_to_clipboard(const std::string& text)
{
	@autoreleasepool
	{
		NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
		[pasteboard clearContents];
		NSString* value = [NSString stringWithUTF8String:text.c_str()];
		return [pasteboard setString:value forType:NSPasteboardTypeString];
	}
}

std::filesystem::path get_default_log_directory()
{
	return std::filesystem::path(NEXUS_MOCK_DATA_DIR) / "arcdps" / "arcdps.cbtlogs";
}

std::filesystem::path read_ini_path_value(const std::filesystem::path& file_path, const std::string& section, const std::string& key)
{
	std::ifstream file(file_path);
	if (!file.is_open())
		return {};

	std::string active_section;
	std::string line;

	while (std::getline(file, line))
	{
		if (line.empty() || line.front() == ';' || line.front() == '#')
			continue;

		if (line.front() == '[' && line.back() == ']')
		{
			active_section = line.substr(1, line.size() - 2);
			continue;
		}

		if (active_section != section)
			continue;

		auto separator = line.find('=');
		if (separator == std::string::npos)
			continue;

		auto name = line.substr(0, separator);
		auto value = line.substr(separator + 1);

		while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
			name.pop_back();
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
			value.erase(value.begin());

		if (name == key)
			return std::filesystem::path(value);
	}

	return {};
}
} // namespace addon::platform
