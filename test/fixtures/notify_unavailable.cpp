// Test-only dynamic-library seam: absence of a notification service alone does
// not make libnotify initialization fail. Never ship this in the runtime image.
extern "C" int notify_init(const char*) { return 0; }
