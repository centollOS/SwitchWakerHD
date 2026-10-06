// AppKit host: typed text for the settings overlay's text prompt (overlay/text_entry.h). While the prompt
// shows, the key monitor (input.mm) hands key presses to an NSTextInputContext instead of reading key
// codes, so the Mac's text input applies as in any text field: the keyboard layout, dead keys (´ then e
// gives é), Option combinations and input methods (Japanese, Chinese; their composition shows at the
// caret, the candidate window opens over the TV window). Editing keys (Return, Escape, Backspace,
// arrows ...) and function keys (F1) stay with overlay::key unless an input method is composing.
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>  // kVK_* key codes

#include "../overlay/overlay.h"
#include "../overlay/text_entry.h"

namespace gfx { void* display_tv_window(); }

@interface WWTextClient : NSObject <NSTextInputClient>
@end

@implementation WWTextClient {
    NSString* _marked;  // input method composition, not yet committed
}
static NSString* plain(id s) { return [s isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)s string] : (NSString*)s; }
- (void)clear {
    _marked = nil;
    text_entry::preedit("");
}
- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    _marked = nil;
    text_entry::preedit("");
    if (NSString* s = plain(string); s.length) text_entry::text(s.UTF8String);
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range {
    (void)sel, (void)range;
    _marked = [plain(string) copy];
    text_entry::preedit(_marked.length ? _marked.UTF8String : "");
}
- (void)unmarkText {  // the input method commits what it shows
    NSString* s = _marked;
    [self clear];
    if (s.length) text_entry::text(s.UTF8String);
}
- (BOOL)hasMarkedText { return _marked.length > 0; }
- (NSRange)markedRange { return _marked.length ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(_marked.length, 0); }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range, (void)actual;
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}
// where the candidate window goes: just above the middle of the TV window (the prompt's field)
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range, (void)actual;
    NSWindow* tv = (__bridge NSWindow*)gfx::display_tv_window();
    NSRect f = tv ? tv.frame : NSScreen.mainScreen.frame;
    return NSMakeRect(NSMidX(f) - 160, NSMidY(f) + 60, 320, 28);
}
// commands the text system makes of keys (insertNewline:, deleteBackward:, ...): the editing keys reach
// the prompt as keys (overlay::key) instead; while composing, the input method keeps them
- (void)doCommandBySelector:(SEL)selector { (void)selector; }
@end

namespace gfx {

// Key monitor (input.mm): true when the text prompt took this key event.
bool text_input_key(void* event) {
    static WWTextClient* client;
    static NSTextInputContext* context;
    static bool activated = false;
    NSEvent* e = (__bridge NSEvent*)event;
    if (!text_entry::active() || overlay::is_open()) {
        if (activated) {  // the prompt is gone (or the menu is over it): leave the text system
            [context discardMarkedText];
            [context deactivate];
            [client clear];
            activated = false;
        }
        return false;
    }
    if (!context) {
        client = [[WWTextClient alloc] init];
        context = [[NSTextInputContext alloc] initWithClient:client];
    }
    if (!activated) {
        [context activate];
        activated = true;
    }
    if (e.type == NSEventTypeFlagsChanged) return false;
    if (!client.hasMarkedText) {
        // editing and function keys (F1 opens the menu) go to overlay::key, key ups too
        if (e.type != NSEventTypeKeyDown || (e.modifierFlags & (NSEventModifierFlagFunction | NSEventModifierFlagControl))) return false;
        switch (e.keyCode) {
        case kVK_Return: case kVK_ANSI_KeypadEnter: case kVK_Escape: case kVK_Delete: case kVK_ForwardDelete:
        case kVK_Tab: case kVK_LeftArrow: case kVK_RightArrow: case kVK_UpArrow: case kVK_DownArrow:
            return false;
        default: break;
        }
    } else if (e.type != NSEventTypeKeyDown) {
        return true;  // composing: key ups belong to the input method's keys
    }
    if (![context handleEvent:e]) {
        // not taken by the text system: plain characters still type
        NSString* s = e.characters;
        if (s.length && [s characterAtIndex:0] >= 0x20 && ([s characterAtIndex:0] < 0xF700 || [s characterAtIndex:0] > 0xF8FF))
            text_entry::text(s.UTF8String);
    }
    return true;
}

}  // namespace gfx
