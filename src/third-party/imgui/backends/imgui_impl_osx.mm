// dear imgui: Platform Backend for OSX / Cocoa
// This needs to be used along with a Renderer (e.g. OpenGL2, OpenGL3, Vulkan, Metal..)
// [ALPHA] Early backend, not well tested. If you want a portable application, prefer using the GLFW or SDL platform Backends on Mac.

// Implemented features:
//  [X] Platform: Mouse cursor shape and visibility. Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
//  [X] Platform: OSX clipboard is supported within core Dear ImGui (no specific code in this backend).
// Issues:
//  [ ] Platform: Keys are all generally very broken. Best using [event keycode] and not [event characters]..

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include "imgui.h"
#include "imgui_impl_osx.h"
#import <Cocoa/Cocoa.h>
#include <mach/mach_time.h>

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2021-09-21: Use mach_absolute_time as CFAbsoluteTimeGetCurrent can jump backwards.
//  2021-08-17: Calling io.AddFocusEvent() on NSApplicationDidBecomeActiveNotification/NSApplicationDidResignActiveNotification events.
//  2021-06-23: Inputs: Added a fix for shortcuts using CTRL key instead of CMD key.
//  2021-04-19: Inputs: Added a fix for keys remaining stuck in pressed state when CMD-tabbing into different application.
//  2021-01-27: Inputs: Added a fix for mouse position not being reported when mouse buttons other than left one are down.
//  2020-10-28: Inputs: Added a fix for handling keypad-enter key.
//  2020-05-25: Inputs: Added a fix for missing trackpad clicks when done with "soft tap".
//  2019-12-05: Inputs: Added support for ImGuiMouseCursor_NotAllowed mouse cursor.
//  2019-10-11: Inputs:  Fix using Backspace key.
//  2019-07-21: Re-added clipboard handlers as they are not enabled by default in core imgui.cpp (reverted 2019-05-18 change).
//  2019-05-28: Inputs: Added mouse cursor shape and visibility support.
//  2019-05-18: Misc: Removed clipboard handlers as they are now supported by core imgui.cpp.
//  2019-05-11: Inputs: Don't filter character values before calling AddInputCharacter() apart from 0xF700..0xFFFF range.
//  2018-11-30: Misc: Setting up io.BackendPlatformName so it can be displayed in the About Window.
//  2018-07-07: Initial version.

@class ImFocusObserver;

// Data
static double         g_HostClockPeriod = 0.0;
static double         g_Time = 0.0;
static NSCursor*      g_MouseCursors[ImGuiMouseCursor_COUNT] = {};
static bool           g_MouseCursorHidden = false;
static bool           g_MouseJustPressed[ImGuiMouseButton_COUNT] = {};
static bool           g_MouseDown[ImGuiMouseButton_COUNT] = {};
static ImFocusObserver* g_FocusObserver = NULL;

// Undocumented methods for creating cursors.
@interface NSCursor()
+ (id)_windowResizeNorthWestSouthEastCursor;
+ (id)_windowResizeNorthEastSouthWestCursor;
+ (id)_windowResizeNorthSouthCursor;
+ (id)_windowResizeEastWestCursor;
@end

static void InitHostClockPeriod()
{
    struct mach_timebase_info info;
    mach_timebase_info(&info);
    g_HostClockPeriod = 1e-9 * ((double)info.denom / (double)info.numer); // Period is the reciprocal of frequency.
}

static double GetMachAbsoluteTimeInSeconds()
{
    return (double)mach_absolute_time() * g_HostClockPeriod;
}

static void resetKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    memset(io.KeysDown, 0, sizeof(io.KeysDown));
    io.KeyCtrl = io.KeyShift = io.KeyAlt = io.KeySuper = false;
}

@interface ImFocusObserver : NSObject

- (void)onApplicationBecomeActive:(NSNotification*)aNotification;
- (void)onApplicationBecomeInactive:(NSNotification*)aNotification;

@end

@implementation ImFocusObserver

- (void)onApplicationBecomeActive:(NSNotification*)aNotification
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(true);
}

- (void)onApplicationBecomeInactive:(NSNotification*)aNotification
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddFocusEvent(false);

    // Unfocused applications do not receive input events, therefore we must manually
    // release any pressed keys when application loses focus, otherwise they would remain
    // stuck in a pressed state. https://github.com/ocornut/imgui/issues/3832
    resetKeys();
}

@end

// Functions
bool ImGui_ImplOSX_Init()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup backend capabilities flags
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;           // We can honor GetMouseCursor() values (optional)
    //io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;          // We can honor io.WantSetMousePos requests (optional, rarely used)
    //io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;    // We can create multi-viewports on the Platform side (optional)
    //io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport; // We can set io.MouseHoveredViewport correctly (optional, not easy)
    io.BackendPlatformName = "imgui_impl_osx";

    // Keyboard mapping. Dear ImGui will use those indices to peek into the io.KeyDown[] array.
    const int offset_for_function_keys = 256 - 0xF700;
    io.KeyMap[ImGuiKey_Tab]             = '\t';
    io.KeyMap[ImGuiKey_LeftArrow]       = NSLeftArrowFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_RightArrow]      = NSRightArrowFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_UpArrow]         = NSUpArrowFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_DownArrow]       = NSDownArrowFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_PageUp]          = NSPageUpFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_PageDown]        = NSPageDownFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_Home]            = NSHomeFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_End]             = NSEndFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_Insert]          = NSInsertFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_Delete]          = NSDeleteFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_Backspace]       = 127;
    io.KeyMap[ImGuiKey_Space]           = 32;
    io.KeyMap[ImGuiKey_Enter]           = 13;
    io.KeyMap[ImGuiKey_Escape]          = 27;
    io.KeyMap[ImGuiKey_Apostrophe]      = '\''; // '
    io.KeyMap[ImGuiKey_Comma]           = ','; // ,
    io.KeyMap[ImGuiKey_Minus]           = '-'; // -
    io.KeyMap[ImGuiKey_Period]          = '.'; // .
    io.KeyMap[ImGuiKey_Slash]           = '/'; // /
    io.KeyMap[ImGuiKey_Semicolon]       = ';'; // ;
    io.KeyMap[ImGuiKey_Equal]           = '='; // =
    io.KeyMap[ImGuiKey_LeftBracket]     = '['; // [
    io.KeyMap[ImGuiKey_Backslash]       = '\\'; // \ (this text inhibit multiline comment caused by backlash)
    io.KeyMap[ImGuiKey_RightBracket]    = ']'; // ]
    io.KeyMap[ImGuiKey_GraveAccent]     = '`'; // `
    io.KeyMap[ImGuiKey_CapsLock]        = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_ScrollLock]      = NSScrollLockFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_NumLock]         = NSClearLineFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_PrintScreen]     = NSPrintScreenFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_Pause]           = NSPauseFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_KeyPad0]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad1]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad2]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad3]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad4]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad5]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad6]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad7]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad8]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPad9]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadDecimal]   = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadDivide]    = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadMultiply]  = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadSubtract]  = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadAdd]       = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_KeyPadEnter]     = 3;
    io.KeyMap[ImGuiKey_KeyPadEqual]     = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_LeftShift]       = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_LeftControl]     = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_LeftAlt]         = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_LeftSuper]       = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_RightShift]      = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_RightControl]    = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_RightAlt]        = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_RightSuper]      = 0; // FIXME: not implemented
    io.KeyMap[ImGuiKey_Menu]            = NSMenuFunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_0]               = '0';
    io.KeyMap[ImGuiKey_1]               = '1';
    io.KeyMap[ImGuiKey_2]               = '2';
    io.KeyMap[ImGuiKey_3]               = '3';
    io.KeyMap[ImGuiKey_4]               = '4';
    io.KeyMap[ImGuiKey_5]               = '5';
    io.KeyMap[ImGuiKey_6]               = '6';
    io.KeyMap[ImGuiKey_7]               = '7';
    io.KeyMap[ImGuiKey_8]               = '8';
    io.KeyMap[ImGuiKey_9]               = '9';
    io.KeyMap[ImGuiKey_A]               = 'A';
    io.KeyMap[ImGuiKey_B]               = 'B';
    io.KeyMap[ImGuiKey_C]               = 'C';
    io.KeyMap[ImGuiKey_D]               = 'D';
    io.KeyMap[ImGuiKey_E]               = 'E';
    io.KeyMap[ImGuiKey_F]               = 'F';
    io.KeyMap[ImGuiKey_G]               = 'G';
    io.KeyMap[ImGuiKey_H]               = 'H';
    io.KeyMap[ImGuiKey_I]               = 'I';
    io.KeyMap[ImGuiKey_J]               = 'J';
    io.KeyMap[ImGuiKey_K]               = 'K';
    io.KeyMap[ImGuiKey_L]               = 'L';
    io.KeyMap[ImGuiKey_M]               = 'M';
    io.KeyMap[ImGuiKey_N]               = 'N';
    io.KeyMap[ImGuiKey_O]               = 'O';
    io.KeyMap[ImGuiKey_P]               = 'P';
    io.KeyMap[ImGuiKey_Q]               = 'Q';
    io.KeyMap[ImGuiKey_R]               = 'R';
    io.KeyMap[ImGuiKey_S]               = 'S';
    io.KeyMap[ImGuiKey_T]               = 'T';
    io.KeyMap[ImGuiKey_U]               = 'U';
    io.KeyMap[ImGuiKey_V]               = 'V';
    io.KeyMap[ImGuiKey_W]               = 'W';
    io.KeyMap[ImGuiKey_X]               = 'X';
    io.KeyMap[ImGuiKey_Y]               = 'Y';
    io.KeyMap[ImGuiKey_Z]               = 'Z';
    io.KeyMap[ImGuiKey_F1]              = NSF1FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F2]              = NSF2FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F3]              = NSF3FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F4]              = NSF4FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F5]              = NSF5FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F6]              = NSF6FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F7]              = NSF7FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F8]              = NSF8FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F9]              = NSF9FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F10]             = NSF10FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F11]             = NSF11FunctionKey + offset_for_function_keys;
    io.KeyMap[ImGuiKey_F12]             = NSF12FunctionKey + offset_for_function_keys;

    // Load cursors. Some of them are undocumented.
    g_MouseCursorHidden = false;
    g_MouseCursors[ImGuiMouseCursor_Arrow] = [NSCursor arrowCursor];
    g_MouseCursors[ImGuiMouseCursor_TextInput] = [NSCursor IBeamCursor];
    g_MouseCursors[ImGuiMouseCursor_ResizeAll] = [NSCursor closedHandCursor];
    g_MouseCursors[ImGuiMouseCursor_Hand] = [NSCursor pointingHandCursor];
    g_MouseCursors[ImGuiMouseCursor_NotAllowed] = [NSCursor operationNotAllowedCursor];
    g_MouseCursors[ImGuiMouseCursor_ResizeNS] = [NSCursor respondsToSelector:@selector(_windowResizeNorthSouthCursor)] ? [NSCursor _windowResizeNorthSouthCursor] : [NSCursor resizeUpDownCursor];
    g_MouseCursors[ImGuiMouseCursor_ResizeEW] = [NSCursor respondsToSelector:@selector(_windowResizeEastWestCursor)] ? [NSCursor _windowResizeEastWestCursor] : [NSCursor resizeLeftRightCursor];
    g_MouseCursors[ImGuiMouseCursor_ResizeNESW] = [NSCursor respondsToSelector:@selector(_windowResizeNorthEastSouthWestCursor)] ? [NSCursor _windowResizeNorthEastSouthWestCursor] : [NSCursor closedHandCursor];
    g_MouseCursors[ImGuiMouseCursor_ResizeNWSE] = [NSCursor respondsToSelector:@selector(_windowResizeNorthWestSouthEastCursor)] ? [NSCursor _windowResizeNorthWestSouthEastCursor] : [NSCursor closedHandCursor];

    // Note that imgui.cpp also include default OSX clipboard handlers which can be enabled
    // by adding '#define IMGUI_ENABLE_OSX_DEFAULT_CLIPBOARD_FUNCTIONS' in imconfig.h and adding '-framework ApplicationServices' to your linker command-line.
    // Since we are already in ObjC land here, it is easy for us to add a clipboard handler using the NSPasteboard api.
    io.SetClipboardTextFn = [](void*, const char* str) -> void
    {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard declareTypes:[NSArray arrayWithObject:NSPasteboardTypeString] owner:nil];
        [pasteboard setString:[NSString stringWithUTF8String:str] forType:NSPasteboardTypeString];
    };

    io.GetClipboardTextFn = [](void*) -> const char*
    {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        NSString* available = [pasteboard availableTypeFromArray: [NSArray arrayWithObject:NSPasteboardTypeString]];
        if (![available isEqualToString:NSPasteboardTypeString])
            return NULL;

        NSString* string = [pasteboard stringForType:NSPasteboardTypeString];
        if (string == nil)
            return NULL;

        const char* string_c = (const char*)[string UTF8String];
        size_t string_len = strlen(string_c);
        static ImVector<char> s_clipboard;
        s_clipboard.resize((int)string_len + 1);
        strcpy(s_clipboard.Data, string_c);
        return s_clipboard.Data;
    };

    g_FocusObserver = [[ImFocusObserver alloc] init];
    [[NSNotificationCenter defaultCenter] addObserver:g_FocusObserver
                                             selector:@selector(onApplicationBecomeActive:)
                                                 name:NSApplicationDidBecomeActiveNotification
                                               object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:g_FocusObserver
                                             selector:@selector(onApplicationBecomeInactive:)
                                                 name:NSApplicationDidResignActiveNotification
                                               object:nil];

    return true;
}

void ImGui_ImplOSX_Shutdown()
{
    g_FocusObserver = NULL;
}

static void ImGui_ImplOSX_UpdateMouseCursorAndButtons()
{
    // Update buttons
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); i++)
    {
        // If a mouse press event came, always pass it as "mouse held this frame", so we don't miss click-release events that are shorter than 1 frame.
        io.MouseDown[i] = g_MouseJustPressed[i] || g_MouseDown[i];
        g_MouseJustPressed[i] = false;
    }

    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    if (io.MouseDrawCursor || imgui_cursor == ImGuiMouseCursor_None)
    {
        // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
        if (!g_MouseCursorHidden)
        {
            g_MouseCursorHidden = true;
            [NSCursor hide];
        }
    }
    else
    {
        // Show OS mouse cursor
        [g_MouseCursors[g_MouseCursors[imgui_cursor] ? imgui_cursor : ImGuiMouseCursor_Arrow] set];
        if (g_MouseCursorHidden)
        {
            g_MouseCursorHidden = false;
            [NSCursor unhide];
        }
    }
}

void ImGui_ImplOSX_NewFrame(NSView* view)
{
    // Setup display size
    ImGuiIO& io = ImGui::GetIO();
    if (view)
    {
        const float dpi = (float)[view.window backingScaleFactor];
        io.DisplaySize = ImVec2((float)view.bounds.size.width, (float)view.bounds.size.height);
        io.DisplayFramebufferScale = ImVec2(dpi, dpi);
    }

    // Setup time step
    if (g_Time == 0.0)
    {
        InitHostClockPeriod();
        g_Time = GetMachAbsoluteTimeInSeconds();
    }
    double current_time = GetMachAbsoluteTimeInSeconds();
    io.DeltaTime = (float)(current_time - g_Time);
    g_Time = current_time;

    ImGui_ImplOSX_UpdateMouseCursorAndButtons();
}

static int mapCharacterToKey(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    if (c == 25) // SHIFT+TAB -> TAB
        return 9;
    if (c >= 0 && c < 256)
        return c;
    if (c >= 0xF700 && c < 0xF700 + 256)
        return c - 0xF700 + 256;
    return -1;
}

bool ImGui_ImplOSX_HandleEvent(NSEvent* event, NSView* view)
{
    ImGuiIO& io = ImGui::GetIO();

    if (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeOtherMouseDown)
    {
        int button = (int)[event buttonNumber];
        if (button >= 0 && button < IM_ARRAYSIZE(g_MouseDown))
            g_MouseDown[button] = g_MouseJustPressed[button] = true;
        return io.WantCaptureMouse;
    }

    if (event.type == NSEventTypeLeftMouseUp || event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeOtherMouseUp)
    {
        int button = (int)[event buttonNumber];
        if (button >= 0 && button < IM_ARRAYSIZE(g_MouseDown))
            g_MouseDown[button] = false;
        return io.WantCaptureMouse;
    }

    if (event.type == NSEventTypeMouseMoved || event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeRightMouseDragged || event.type == NSEventTypeOtherMouseDragged)
    {
        NSPoint mousePoint = event.locationInWindow;
        mousePoint = [view convertPoint:mousePoint fromView:nil];
        mousePoint = NSMakePoint(mousePoint.x, view.bounds.size.height - mousePoint.y);
        io.MousePos = ImVec2((float)mousePoint.x, (float)mousePoint.y);
    }

    if (event.type == NSEventTypeScrollWheel)
    {
        double wheel_dx = 0.0;
        double wheel_dy = 0.0;

        #if MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
        if (floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_6)
        {
            wheel_dx = [event scrollingDeltaX];
            wheel_dy = [event scrollingDeltaY];
            if ([event hasPreciseScrollingDeltas])
            {
                wheel_dx *= 0.1;
                wheel_dy *= 0.1;
            }
        }
        else
        #endif // MAC_OS_X_VERSION_MAX_ALLOWED
        {
            wheel_dx = [event deltaX];
            wheel_dy = [event deltaY];
        }

        if (fabs(wheel_dx) > 0.0)
            io.MouseWheelH += (float)wheel_dx * 0.1f;
        if (fabs(wheel_dy) > 0.0)
            io.MouseWheel += (float)wheel_dy * 0.1f;
        return io.WantCaptureMouse;
    }

    // FIXME: All the key handling is wrong and broken. Refer to GLFW's cocoa_init.mm and cocoa_window.mm.
    if (event.type == NSEventTypeKeyDown)
    {
        NSString* str = [event characters];
        NSUInteger len = [str length];
        for (NSUInteger i = 0; i < len; i++)
        {
            int c = [str characterAtIndex:i];
            if (!io.KeySuper && !(c >= 0xF700 && c <= 0xFFFF) && c != 127)
                io.AddInputCharacter((unsigned int)c);

            // We must reset in case we're pressing a sequence of special keys while keeping the command pressed
            int key = mapCharacterToKey(c);
            if (key != -1 && key < 256 && !io.KeySuper)
                resetKeys();
            if (key != -1)
                io.KeysDown[key] = true;
        }
        return io.WantCaptureKeyboard;
    }

    if (event.type == NSEventTypeKeyUp)
    {
        NSString* str = [event characters];
        NSUInteger len = [str length];
        for (NSUInteger i = 0; i < len; i++)
        {
            int c = [str characterAtIndex:i];
            int key = mapCharacterToKey(c);
            if (key != -1)
                io.KeysDown[key] = false;
        }
        return io.WantCaptureKeyboard;
    }

    if (event.type == NSEventTypeFlagsChanged)
    {
        unsigned int flags = [event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;

        bool oldKeyCtrl = io.KeyCtrl;
        bool oldKeyShift = io.KeyShift;
        bool oldKeyAlt = io.KeyAlt;
        bool oldKeySuper = io.KeySuper;
        io.KeyCtrl      = flags & NSEventModifierFlagControl;
        io.KeyShift     = flags & NSEventModifierFlagShift;
        io.KeyAlt       = flags & NSEventModifierFlagOption;
        io.KeySuper     = flags & NSEventModifierFlagCommand;

        // We must reset them as we will not receive any keyUp event if they where pressed with a modifier
        if ((oldKeyShift && !io.KeyShift) || (oldKeyCtrl && !io.KeyCtrl) || (oldKeyAlt && !io.KeyAlt) || (oldKeySuper && !io.KeySuper))
            resetKeys();
        return io.WantCaptureKeyboard;
    }

    return false;
}