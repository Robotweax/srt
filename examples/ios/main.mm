/* SPDX-License-Identifier: MIT */
#import <UIKit/UIKit.h>
#include <srt/srt.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <string>

@interface DemoController : UIViewController {
    std::atomic<bool> cancelled;
    BOOL running;
}
@property UITextField *hostField;
@property UITextField *portField;
@property UITextField *keyField;
@property UISegmentedControl *crypto;
@property UIButton *connectButton;
@property UILabel *status;
@end

@implementation DemoController
- (UITextField *)field:(NSString *)placeholder value:(NSString *)value {
    UITextField *field = [UITextField new];
    field.placeholder = placeholder;
    field.text = value;
    field.borderStyle = UITextBorderStyleRoundedRect;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    return field;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    self.hostField = [self field:@"Peer IPv4 address" value:env[@"SRT_DEMO_HOST"] ?: @"127.0.0.1"];
    self.portField = [self field:@"UDP port" value:env[@"SRT_DEMO_PORT"] ?: @"24580"];
    self.portField.keyboardType = UIKeyboardTypeNumberPad;
    self.keyField = [self field:@"Test passphrase (10–79 ASCII bytes)" value:env[@"SRT_DEMO_KEY"] ?: @""];
    self.keyField.secureTextEntry = YES;
    self.crypto = [[UISegmentedControl alloc] initWithItems:@[@"AES-CTR", @"AES-GCM"]];
    self.crypto.selectedSegmentIndex = [env[@"SRT_DEMO_CRYPTO"] isEqual:@"ctr"] ? 0 : 1;
    self.connectButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [self.connectButton setTitle:@"Connect & verify echo" forState:UIControlStateNormal];
    [self.connectButton addTarget:self action:@selector(start) forControlEvents:UIControlEventTouchUpInside];
    UIButton *stop = [UIButton buttonWithType:UIButtonTypeSystem];
    [stop setTitle:@"Disconnect" forState:UIControlStateNormal];
    [stop addTarget:self action:@selector(stop) forControlEvents:UIControlEventTouchUpInside];
    self.status = [UILabel new];
    self.status.numberOfLines = 0;
    self.status.text = @"Idle — experimental caller-only echo test";
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.hostField, self.portField, self.keyField, self.crypto, self.connectButton, stop, self.status]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:24],
        [stack.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-20]]];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(stop) name:UIApplicationDidEnterBackgroundNotification object:nil];
    if ([env[@"SRT_DEMO_AUTORUN"] isEqual:@"1"]) {
        dispatch_async(dispatch_get_main_queue(), ^{ [self start]; });
    }
}
- (void)stop {
    if (running) {
        cancelled.store(true);
        self.status.text = @"Disconnecting (bounded by connection/I/O timeout)…";
    }
}
- (void)start {
    if (running) return;
    [self.view endEditing:YES];
    NSString *host = self.hostField.text;
    NSString *portText = self.portField.text;
    NSScanner *scanner = [NSScanner scannerWithString:portText];
    int port = 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    NSData *key = [self.keyField.text dataUsingEncoding:NSASCIIStringEncoding allowLossyConversion:NO];
    if (![scanner scanInt:&port] || !scanner.isAtEnd || port < 1 || port > 65535 ||
        inet_pton(AF_INET, host.UTF8String, &address.sin_addr) != 1 || key.length < 10 || key.length > 79) {
        self.status.text = @"Enter a numeric IPv4 address, port 1–65535 and a 10–79 byte ASCII passphrase.";
        return;
    }
    address.sin_port = htons(static_cast<uint16_t>(port));
    const int mode = self.crypto.selectedSegmentIndex == 0 ? 1 : 2;
    running = YES;
    cancelled.store(false);
    self.connectButton.enabled = NO;
    self.status.text = @"Connecting…";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        // One worker exclusively owns startup, socket I/O, close and cleanup.
        // The UI only publishes cancellation; it never closes a reused handle.
        NSString *result = @"FAIL: startup";
        if (srt_startup() != SRT_ERROR) {
            SRTSOCKET socket = srt_create_socket();
            const int timeout = 3000;
            const int keyLength = 32;
            bool ok = socket != SRT_INVALID_SOCK;
            auto option = [&](SRT_SOCKOPT name, const void *value, int size) {
                if (ok) ok = srt_setsockflag(socket, name, value, size) != SRT_ERROR;
            };
            option(SRTO_CONNTIMEO, &timeout, sizeof(timeout));
            option(SRTO_RCVTIMEO, &timeout, sizeof(timeout));
            option(SRTO_SNDTIMEO, &timeout, sizeof(timeout));
            option(SRTO_PBKEYLEN, &keyLength, sizeof(keyLength));
            option(SRTO_PASSPHRASE, key.bytes, static_cast<int>(key.length));
            option(SRTO_CRYPTOMODE, &mode, sizeof(mode));
            if (ok && !cancelled.load()) ok = srt_connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != SRT_ERROR;
            std::string payload = "Robotweax iOS app encrypted echo";
            auto exchange = [&](const std::string &message) {
                if (cancelled.load() || !ok) return false;
                if (srt_sendmsg(socket, message.data(), static_cast<int>(message.size()), -1, 0) != static_cast<int>(message.size())) return false;
                char buffer[65536];
                const int size = srt_recvmsg(socket, buffer, sizeof(buffer));
                return size == static_cast<int>(message.size()) && std::memcmp(buffer, message.data(), message.size()) == 0;
            };
            ok = exchange(payload);
            SRT_TRACEBSTATS stats{};
            if (ok) ok = srt_bistats(socket, &stats, 0, 1) != SRT_ERROR;
            if (ok) ok = exchange("__robotweax_srt_demo_complete__");
            if (cancelled.load()) result = @"CANCELLED";
            else if (ok) result = [NSString stringWithFormat:@"PASS: encrypted echo verified\nRTT %.3f ms\nSent %lld / received %lld packets", stats.msRTT, (long long)stats.pktSentTotal, (long long)stats.pktRecvTotal];
            else result = [NSString stringWithFormat:@"FAIL: %s (or echo mismatch)", srt_getlasterror_str()];
            if (socket != SRT_INVALID_SOCK) srt_close(socket);
            srt_cleanup();
        }
        // Non-secret result for automation; never persist the passphrase.
        NSString *directory = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        [result writeToFile:[directory stringByAppendingPathComponent:@"result.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            running = NO;
            self.connectButton.enabled = YES;
            self.status.text = result;
        });
    });
}
@end

@interface DemoDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation DemoDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [DemoController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(DemoDelegate.class)); }
}
