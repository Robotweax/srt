/* SPDX-License-Identifier: MIT */
#import <UIKit/UIKit.h>
#include "compat/submillisecond_pacing_platform.hpp"
#include <arpa/inet.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int rwx_mobile_pacing_peer_main(int, char**);
namespace {
struct Resources {
    rusage cpu {};
    task_power_info_data_t power {};
    bool cpuValid = false;
    kern_return_t powerError = KERN_FAILURE;
    Resources() {
        cpuValid = getrusage(RUSAGE_SELF, &cpu) == 0;
        mach_msg_type_number_t count = TASK_POWER_INFO_COUNT;
        powerError = task_info(mach_task_self(), TASK_POWER_INFO,
            reinterpret_cast<task_info_t>(&power), &count);
    }
    double cpuSeconds() const {
        return static_cast<double>(cpu.ru_utime.tv_sec + cpu.ru_stime.tv_sec)
            + static_cast<double>(cpu.ru_utime.tv_usec + cpu.ru_stime.tv_usec) / 1e6;
    }
};
struct Streams {
    std::streambuf* out;
    std::streambuf* err;
    explicit Streams(std::ostream& stream)
        : out(std::cout.rdbuf(stream.rdbuf())), err(std::cerr.rdbuf(stream.rdbuf())) {}
    ~Streams() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
};
}

@interface PacingController : UIViewController
@property UITextField *host;
@property UITextField *port;
@property UISegmentedControl *rate;
@property UISegmentedControl *payload;
@property UISegmentedControl *duration;
@property UIButton *startButton;
@property UILabel *status;
@property UILabel *command;
@property BOOL running;
@property BOOL backgrounded;
@end

@implementation PacingController
- (UITextField *)field:(NSString *)placeholder value:(NSString *)value {
    UITextField *field = [UITextField new];
    field.placeholder = placeholder; field.text = value;
    field.borderStyle = UITextBorderStyleRoundedRect;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    return field;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    self.host = [self field:@"Desktop peer IPv4 address" value:env[@"SRT_PACING_HOST"] ?: @""];
    self.port = [self field:@"UDP port" value:env[@"SRT_PACING_PORT"] ?: @"24583"];
    self.port.keyboardType = UIKeyboardTypeNumberPad;
    self.rate = [[UISegmentedControl alloc] initWithItems:@[@"2 Mbit/s", @"5 Mbit/s", @"8 Mbit/s"]];
    self.rate.selectedSegmentIndex = [env[@"SRT_PACING_BPS"] isEqual:@"8000000"] ? 2 : [env[@"SRT_PACING_BPS"] isEqual:@"5000000"] ? 1 : 0;
    self.payload = [[UISegmentedControl alloc] initWithItems:@[@"188 bytes", @"1316 bytes"]];
    self.payload.selectedSegmentIndex = [env[@"SRT_PACING_BYTES"] isEqual:@"1316"] ? 1 : 0;
    self.duration = [[UISegmentedControl alloc] initWithItems:@[@"10 seconds", @"30 seconds"]];
    self.duration.selectedSegmentIndex = [env[@"SRT_PACING_SECONDS"] isEqual:@"30"] ? 1 : 0;
    self.startButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.startButton setTitle:@"Send measurement traffic" forState:UIControlStateNormal];
    [self.startButton addTarget:self action:@selector(start) forControlEvents:UIControlEventTouchUpInside];
    self.status = [UILabel new]; self.status.numberOfLines = 0;
    self.status.text = ROBOTWEAX_SRT_MOBILE_TIMER_PACING ? @"Candidate: mobile timer waits, no coarse-timer pacing spin" : @"Baseline: legacy mobile pacing";
    self.command = [UILabel new]; self.command.numberOfLines = 0;
    self.command.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    self.command.text = @"Cleartext synthetic measurement only. Keep this app foreground and the desktop receiver log.\nStart receiver first; failed or backgrounded trials are invalid.";
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.status, self.host, self.port, self.rate, self.payload, self.duration, self.startButton, self.command]];
    stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:20],
        [stack.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-16]]];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(background) name:UIApplicationDidEnterBackgroundNotification object:nil];
    if ([env[@"SRT_PACING_AUTORUN"] isEqual:@"1"])
        dispatch_async(dispatch_get_main_queue(), ^{ [self start]; });
}
- (void)background { if (self.running) self.backgrounded = YES; }
- (void)start {
    if (self.running) return;
    [self.view endEditing:YES];
    sockaddr_in address {};
    NSInteger port = self.port.text.integerValue;
    if (inet_pton(AF_INET, self.host.text.UTF8String, &address.sin_addr) != 1 || port < 1 || port > 65535) {
        self.status.text = @"Enter numeric desktop IPv4 and port 1–65535."; return;
    }
    if (NSProcessInfo.processInfo.thermalState >= NSProcessInfoThermalStateSerious) {
        self.status.text = @"Wait for the phone to cool before measuring."; return;
    }
    const std::uint64_t rates[] {2'000'000, 5'000'000, 8'000'000};
    const auto bps = rates[self.rate.selectedSegmentIndex];
    const std::uint64_t bytes = self.payload.selectedSegmentIndex == 0 ? 188 : 1316;
    const std::uint64_t seconds = self.duration.selectedSegmentIndex == 0 ? 10 : 30;
    const auto messages = bps * seconds / (bytes * 8);
    // MAXBW paces encoded SRT datagram bytes, including its 16-byte header.
    const auto bandwidth = (bps * (bytes + 16) + bytes * 8 - 1) / (bytes * 8);
    const auto timeout = seconds * 3'000 + 5'000;
    const std::string host(self.host.text.UTF8String);
    std::vector<std::string> arguments {"pacing", "caller", "--host", host,
        "--port", std::to_string(port), "--connections", "1", "--messages", std::to_string(messages),
        "--message-size", std::to_string(bytes), "--max-bandwidth", std::to_string(bandwidth),
        "--latency-ms", "120", "--timeout-ms", std::to_string(timeout), "--shutdown-grace-ms", "300"};
    self.command.text = [NSString stringWithFormat:@"Desktop receiver:\nrobotweax_srt_scalability_peer listener --host 0.0.0.0 --port %ld --connections 1 --messages %llu --message-size %llu --latency-ms 120 --timeout-ms %llu", (long)port, (unsigned long long)messages, (unsigned long long)bytes, (unsigned long long)timeout];
    self.running = YES; self.backgrounded = NO; self.startButton.enabled = NO;
    const BOOL previousIdle = UIApplication.sharedApplication.idleTimerDisabled;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    const auto thermalBefore = NSProcessInfo.processInfo.thermalState;
    const BOOL lowPowerBefore = NSProcessInfo.processInfo.lowPowerModeEnabled;
    NSString *run = NSUUID.UUID.UUIDString;
    NSString *directory = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *logPath = [directory stringByAppendingPathComponent:[run stringByAppendingString:@".jsonl"]];
    self.status.text = @"Measuring; keep foreground. Network timeout bounds failures.";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        std::ofstream log(logPath.UTF8String);
        int result = -1;
        Resources before;
        const auto started = std::chrono::steady_clock::now();
        if (log) {
            Streams redirected(log);
            try { result = rwx_mobile_pacing_peer_main(static_cast<int>(argv.size()), argv.data()); }
            catch (const std::exception& error) { std::cerr << "measurement exception: " << error.what() << '\n'; }
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Resources after;
        log.close();
        const BOOL powerValid = before.powerError == KERN_SUCCESS && after.powerError == KERN_SUCCESS
            && after.power.task_interrupt_wakeups >= before.power.task_interrupt_wakeups
            && after.power.task_platform_idle_wakeups >= before.power.task_platform_idle_wakeups
            && after.power.task_timer_wakeups_bin_1 >= before.power.task_timer_wakeups_bin_1
            && after.power.task_timer_wakeups_bin_2 >= before.power.task_timer_wakeups_bin_2;
        struct utsname system {}; uname(&system);
        NSMutableDictionary *report = [@{@"schema": @1, @"run": run,
            @"variant": ROBOTWEAX_SRT_MOBILE_TIMER_PACING ? @"timer-no-spin" : @"legacy",
            @"simulator": @(TARGET_OS_SIMULATOR), @"machine": @(system.machine),
            @"os_version": UIDevice.currentDevice.systemVersion,
            @"target_payload_bps": @(bps), @"payload_bytes": @(bytes), @"messages": @(messages),
            @"max_bandwidth_bytes_per_second": @(bandwidth), @"elapsed_seconds": @(elapsed),
            @"peer_exit_code": @(result), @"thermal_before": @(thermalBefore),
            @"thermal_after": @(NSProcessInfo.processInfo.thermalState),
            @"low_power_before": @(lowPowerBefore),
            @"low_power_after": @(NSProcessInfo.processInfo.lowPowerModeEnabled),
            @"task_power_info_available": @(powerValid),
            @"task_power_before_error": @(before.powerError),
            @"task_power_after_error": @(after.powerError),
            @"peer_log": logPath.lastPathComponent,
            @"cpu_seconds": before.cpuValid && after.cpuValid ? @(after.cpuSeconds() - before.cpuSeconds()) : NSNull.null,
            @"interrupt_wakeups": powerValid ? @(after.power.task_interrupt_wakeups - before.power.task_interrupt_wakeups) : NSNull.null,
            @"platform_idle_wakeups": powerValid ? @(after.power.task_platform_idle_wakeups - before.power.task_platform_idle_wakeups) : NSNull.null,
            @"timer_wakeups_bin_1": powerValid ? @(after.power.task_timer_wakeups_bin_1 - before.power.task_timer_wakeups_bin_1) : NSNull.null,
            @"timer_wakeups_bin_2": powerValid ? @(after.power.task_timer_wakeups_bin_2 - before.power.task_timer_wakeups_bin_2) : NSNull.null} mutableCopy];
        dispatch_async(dispatch_get_main_queue(), ^{
            report[@"backgrounded"] = @(self.backgrounded);
            report[@"sender_completed"] = @(result == 0 && !self.backgrounded);
            NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
            [json writeToFile:[directory stringByAppendingPathComponent:[run stringByAppendingString:@".json"]] atomically:YES];
            [json writeToFile:[directory stringByAppendingPathComponent:@"latest.json"] atomically:YES];
            self.running = NO; self.startButton.enabled = YES;
            UIApplication.sharedApplication.idleTimerDisabled = previousIdle;
            self.status.text = [NSString stringWithFormat:@"%@ — %.2f s. Pair with receiver integrity result.\nReport: %@", result == 0 && !self.backgrounded ? @"Sender complete" : @"Invalid trial", elapsed, run];
        });
    });
}
@end

@interface PacingDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation PacingDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [PacingController new];
    [self.window makeKeyAndVisible]; return YES;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(PacingDelegate.class)); }
}
