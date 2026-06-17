#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "addon.h"
#include "dps_report_uploader.h"
#include "evtc.h"
#include "log_manager.h"
#include "mock_api.h"
#include "ui.h"
#include "wingman_uploader.h"

#include <imgui.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_osx.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

extern "C" AddonDefinition_t* GetAddonDef();

static std::filesystem::path write_sample_evtc()
{
	auto directory = addon::directory / "mock-arcdps" / "arcdps.cbtlogs" / "Vale Guardian";
	std::filesystem::create_directories(directory);

	auto file_name = "mock-" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + ".evtc";
	auto path = directory / file_name;

	std::vector<unsigned char> bytes(16, 0);
	bytes[0] = 'E';
	bytes[1] = 'V';
	bytes[2] = 'T';
	bytes[3] = 'C';
	bytes[4] = '2';
	bytes[5] = '0';
	bytes[6] = '2';
	bytes[7] = '4';

	auto trigger_id = static_cast<uint16_t>(TriggerID::ValeGuardian);
	std::memcpy(bytes.data() + 13, &trigger_id, sizeof(trigger_id));

	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return path;
}

static void inject_sample_log()
{
	auto path = write_sample_evtc();
	addon::log_manager->add_log(path);
	addon::ui->logs_table.set_visible(true);
}

static void render_mock_controls()
{
	ImGui::Begin("Mock Controls");

	if (ImGui::Button("Add sample log"))
		inject_sample_log();

	ImGui::SameLine();
	if (ImGui::Button("Toggle addon UI"))
		mac_mock::invoke_keybind("Open Log Uploader", false);

	bool in_combat = mac_mock::state().mumble_data.Context.IsInCombat;
	if (ImGui::Checkbox("In combat", &in_combat))
		mac_mock::state().mumble_data.Context.IsInCombat = in_combat;

	ImGui::Checkbox("Addon options", &mac_mock::state().options_open);
	ImGui::TextUnformatted("Uploads and parsing are mocked in this native dev host.");
	ImGui::End();
}

template <typename Predicate>
static bool wait_for_mock_state(Predicate predicate, std::chrono::seconds timeout)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	return predicate();
}

static int run_smoke_test()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	auto* definition = GetAddonDef();
	if (definition == nullptr || definition->Load == nullptr || definition->Unload == nullptr)
		return 2;

	definition->Load(mac_mock::create_api(ImGui::GetCurrentContext()));
	inject_sample_log();

	std::shared_ptr<Log> log;
	auto parsed = wait_for_mock_state(
	    [&] {
		    std::shared_lock logs_lock(addon::log_manager->logs_mutex);
		    if (addon::log_manager->logs.empty())
			    return false;

		    log = addon::log_manager->logs.front();
		    std::shared_lock log_lock(log->mutex);
		    return log->parser_data.status == ParseStatus::PARSED;
	    },
	    std::chrono::seconds(5));

	if (!parsed)
	{
		std::cerr << "Mock parser did not produce a parsed log\n";
		definition->Unload();
		ImGui::DestroyContext();
		return 3;
	}

	addon::dps_report_uploader->add_log(log);
	auto dps_uploaded = wait_for_mock_state(
	    [&] {
		    std::shared_lock lock(log->mutex);
		    return log->dps_report_upload.status == UploadStatus::UPLOADED && log->dps_report_upload.url.rfind("mock://", 0) == 0;
	    },
	    std::chrono::seconds(5));

	addon::wingman_uploader->add_log(log);
	auto wingman_uploaded = wait_for_mock_state(
	    [&] {
		    std::shared_lock lock(log->mutex);
		    return log->wingman_upload.status == UploadStatus::UPLOADED;
	    },
	    std::chrono::seconds(5));

	definition->Unload();
	ImGui::DestroyContext();

	if (!dps_uploaded)
	{
		std::cerr << "Mock dps.report upload did not complete\n";
		return 4;
	}

	if (!wingman_uploaded)
	{
		std::cerr << "Mock Wingman upload did not complete\n";
		return 5;
	}

	std::cout << "Mock smoke test passed\n";
	return 0;
}

@interface MockViewController : NSViewController <MTKViewDelegate>
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@end

@implementation MockViewController

- (instancetype)init
{
	self = [super initWithNibName:nil bundle:nil];
	if (self)
	{
		_device = MTLCreateSystemDefaultDevice();
		_commandQueue = [_device newCommandQueue];
	}
	return self;
}

- (void)loadView
{
	self.view = [[MTKView alloc] initWithFrame:CGRectMake(0, 0, 1280, 800)];
}

- (void)viewDidLoad
{
	[super viewDidLoad];

	MTKView* view = (MTKView*)self.view;
	view.device = self.device;
	view.delegate = self;
	view.clearColor = MTLClearColorMake(0.08, 0.09, 0.1, 1.0);

	NSTrackingArea* trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
	                                                            options:NSTrackingMouseMoved | NSTrackingInVisibleRect | NSTrackingActiveAlways
	                                                              owner:self
	                                                           userInfo:nil];
	[self.view addTrackingArea:trackingArea];

	NSEventMask eventMask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged | NSEventTypeScrollWheel;
	[NSEvent addLocalMonitorForEventsMatchingMask:eventMask
	                                      handler:^NSEvent* _Nullable(NSEvent* event) {
		                                      ImGui_ImplOSX_HandleEvent(event, self.view);
		                                      return event;
	                                      }];

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplMetal_Init(self.device);
	ImGui_ImplOSX_Init();

	auto* definition = GetAddonDef();
	if (definition != nullptr && definition->Load != nullptr)
		definition->Load(mac_mock::create_api(ImGui::GetCurrentContext()));

	addon::ui->logs_table.set_visible(true);
}

- (void)mouseMoved:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)mouseDown:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)rightMouseDown:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)otherMouseDown:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)mouseUp:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)rightMouseUp:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)otherMouseUp:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)mouseDragged:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)rightMouseDragged:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)otherMouseDragged:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }
- (void)scrollWheel:(NSEvent*)event { ImGui_ImplOSX_HandleEvent(event, self.view); }

- (void)drawInMTKView:(MTKView*)view
{
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(view.bounds.size.width, view.bounds.size.height);
	CGFloat scale = view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
	io.DisplayFramebufferScale = ImVec2(scale, scale);
	io.DeltaTime = 1.0f / float(view.preferredFramesPerSecond ?: 60);

	id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];
	MTLRenderPassDescriptor* renderPassDescriptor = view.currentRenderPassDescriptor;

	if (renderPassDescriptor != nil)
	{
		id<MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

		ImGui_ImplMetal_NewFrame(renderPassDescriptor);
		ImGui_ImplOSX_NewFrame(view);
		ImGui::NewFrame();

		render_mock_controls();
		mac_mock::render_quick_access();

		for (auto callback : mac_mock::state().render_callbacks)
			callback();

		if (mac_mock::state().options_open)
		{
			ImGui::Begin("Addon Options", &mac_mock::state().options_open);
			for (auto callback : mac_mock::state().options_render_callbacks)
				callback();
			ImGui::End();
		}

		ImGui::Render();
		ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderEncoder);

		[renderEncoder endEncoding];
		[commandBuffer presentDrawable:view.currentDrawable];
	}

	[commandBuffer commit];
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}

- (void)dealloc
{
	auto* definition = GetAddonDef();
	if (definition != nullptr && definition->Unload != nullptr)
		definition->Unload();

	ImGui_ImplMetal_Shutdown();
	ImGui_ImplOSX_Shutdown();
	ImGui::DestroyContext();
}

@end

@interface MockAppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow* window;
@end

@implementation MockAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
	auto* controller = [[MockViewController alloc] init];
	self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 800)
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
	                                            backing:NSBackingStoreBuffered
	                                              defer:NO];
	self.window.title = @"Nexus Log Uploader Mock";
	self.window.contentViewController = controller;
	[self.window center];
	[self.window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
	return YES;
}

@end

int main(int argc, const char** argv)
{
	if (argc > 1 && std::string(argv[1]) == "--smoke-test")
		return run_smoke_test();

	@autoreleasepool
	{
		NSApplication* app = [NSApplication sharedApplication];
		MockAppDelegate* delegate = [[MockAppDelegate alloc] init];
		[app setDelegate:delegate];
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app run];
	}

	return 0;
}
