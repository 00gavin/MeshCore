#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#ifdef WIFI_SSID
  #include <WiFi.h>
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   3000   // 3 seconds

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

// the READ page can send one of these to whichever channel/contact is being viewed
static const char* const UI_SEND_MESSAGES[] = {
  "Yo", "Ping", "Bye", "Yes", "No", "OK", "On my way", "Here", "HELP", "Hold position",
  "All clear", "Check in", "ETA 5m", "ETA 15m", "Delayed", "Stopped", "Meet here",
  "At waypoint", "Moving", "Regroup", "Found sign", "Target located", "Lost",
};
#define UI_SEND_NUM_MESSAGES  ((int)(sizeof(UI_SEND_MESSAGES) / sizeof(UI_SEND_MESSAGES[0])))

// Total picker capacity, and per-source caps. Each source gets its own budget so that a busy
// mesh (or a long channel list) can't crowd the others out of the picker entirely.
#ifndef UI_TARGET_LIST_SIZE
  #define UI_TARGET_LIST_SIZE  20
#endif
#ifndef UI_TARGET_CHANNEL_MAX
  #define UI_TARGET_CHANNEL_MAX  6
#endif
#ifndef UI_TARGET_RECENT_MAX
  #define UI_TARGET_RECENT_MAX  8
#endif
#define UI_PICK_VISIBLE_ROWS  4   // rows that fit under the title bar
#ifndef UI_PICK_TIMEOUT_MILLIS
  #define UI_PICK_TIMEOUT_MILLIS  8000   // close an idle picker without acting on it
#endif

// the READ page shows recent messages for one channel or contact
#ifndef UI_READ_MSG_COUNT
  #define UI_READ_MSG_COUNT  16
#endif
#ifndef UI_READ_SCROLL_MILLIS
  #define UI_READ_SCROLL_MILLIS  1000   // advance the text by one step this often
#endif
#ifndef UI_READ_TOP_MILLIS
  #define UI_READ_TOP_MILLIS  4000   // but hold longer at the top, on the newest message
#endif
#ifndef UI_READ_BOTTOM_MILLIS
  #define UI_READ_BOTTOM_MILLIS  2000   // and at the bottom, before wrapping round again
#endif
// The message view hides the title bar and uses the full height, so the row count comes from
// the display rather than being fixed. 11px matches the line spacing used elsewhere.
#define UI_READ_LINE_HEIGHT  11
// Scrolling is by pixels, not whole lines, so the text creeps rather than jumping a line at a
// time. Half a line per step keeps the average speed close to a line every two seconds.
#define UI_READ_SCROLL_STEP  (UI_READ_LINE_HEIGHT / 2)
#define UI_READ_SCROLLBAR_W  2   // scrollbar down the right edge of the message view
// How many conversations can be remembered as "read up to here", so the target picker can mark
// the ones with something new. Only conversations actually opened need a slot, so a handful
// covers normal use; the least recently marked is dropped when it overflows.
#ifndef UI_READ_MARK_COUNT
  #define UI_READ_MARK_COUNT  8
#endif

// millis() wraps around every ~49 days. Comparing the difference as a signed value keeps a
// deadline working across the wrap, where a plain millis() >= deadline would stall until it
// came round again.
static inline bool uiTimePassed(unsigned long deadline) {
  return (long)(millis() - deadline) >= 0;
}

#include "icons.h"

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  SplashScreen(UITask* task) : _task(task) {
    // strip off dash and commit hash by changing dash to null terminator
    // e.g: v1.2.3-abcdef -> v1.2.3
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    // meshcore logo
    display.setColor(UIColor::corp_blue);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    uint16_t websiteWidth = display.getTextWidth(website);
    display.setCursor((display.width() - websiteWidth) / 2, 22);
    display.print(website);

    // version info
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 35, _version_info);

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 48, FIRMWARE_BUILD_DATE);

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  enum HomePage {
    STATUS,
    RECENT,
    RADIO,
    BLUETOOTH,
    ADVERT,
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
    SHUTDOWN,
    READ,      // last page in the carousel
    Count      // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;
  bool _shutdown_init;
  AdvertPath recent[UI_RECENT_LIST_SIZE];

  struct PickTarget {
    char    name[32];
    uint8_t pubkey_prefix[7];   // contacts only
    uint8_t channel_idx;        // channels only
    bool    is_channel;
  };
  int _num_targets;                    // targets in the snapshot
  PickTarget _targets[UI_TARGET_LIST_SIZE];
  // scratch for getRecentlyHeard(). A member rather than a local: the Arduino loop task only
  // gets a 4KB stack, and AdvertPath is ~108 bytes apiece.
  AdvertPath _recent_scratch[UI_TARGET_RECENT_MAX];

  // READ page state: view the conversation, or pick a target to read, or pick a message to
  // send to it. Neither picker has a "Back" row, so a selection indexes its list directly.
  // The pickers run with multi-click detection off (see wantsFastClicks), so on single-button
  // hardware the way out without acting is to let the picker time out; boards with a separate
  // left/back button can still use that, as it is not subject to click coalescing.
  enum ReadStage { READ_VIEW = 0, READ_PICK_TARGET, READ_PICK_MSG };
  uint8_t _read_stage;
  int  _read_sel;                      // index into _targets
  int  _read_msg_sel;                  // index into UI_SEND_MESSAGES
  unsigned long _read_pick_expiry;     // when an idle picker gives up and closes
  int  _read_scroll;                   // how far down the conversation the view has crept, px
  unsigned long _read_next_scroll;     // when the text next advances a step
  bool _read_timing_set;               // _read_next_scroll holds a real deadline
  bool _read_scrolling;                // there is more text than fits, so keep re-rendering
  int  _read_cycles_left;              // full scrolls still owed before sleep is allowed again
  PickTarget _read_target;             // what is being read (lazily defaulted to Public)
  // Indices into the mesh's history ring rather than copies of the entries: an entry is a little
  // over 200 bytes, so holding 16 of them here would cost several KB of RAM permanently, and
  // re-copying them on every render would burn that much memcpy a second while scrolling.
  uint8_t _read_idx[UI_READ_MSG_COUNT];
  int  _read_num;
  uint32_t _read_version;              // history version _read_idx was built from
  bool _read_fetched;                  // _read_idx holds a fetch for the current target
  uint32_t _read_now;                  // the clock every displayed age is measured against
  // Wrapped line count for the conversation as it currently reads, or -1 when it needs working
  // out again. Only the history changing or the pinned clock moving can alter it, and both are
  // rare next to the once-a-second re-render, so this saves a whole measuring pass per frame.
  int  _read_total_lines;

  // How far each conversation has been read, so the picker can mark the ones with something
  // newer. Oldest mark is recycled once the table is full.
  struct ReadMark {
    uint8_t  pubkey_prefix[7];
    uint8_t  channel_idx;
    bool     is_channel;
    bool     used;
    uint32_t seen;                     // recv_timestamp of the newest message shown
  };
  ReadMark _read_marks[UI_READ_MARK_COUNT];

  // How long the current line stays put. Both ends of the conversation dwell longer than the
  // lines in between: the top so the newest message can be read without catching it
  // mid-scroll, the bottom so the oldest isn't whipped away as it wraps round.
  unsigned long readHoldMillis(int max_scroll) const {
    if (_read_scroll <= 0) return UI_READ_TOP_MILLIS;
    if (_read_scroll >= max_scroll) return UI_READ_BOTTOM_MILLIS;
    return UI_READ_SCROLL_MILLIS;
  }

  // Pin the clock that the displayed ages are worked out from. They are only allowed to move
  // when the view is back at the top: an age is part of the text, so letting it tick while the
  // conversation is part-way through a pass would re-wrap lines under the reader -- "59s"
  // becoming "1m" is a character shorter, and that is enough to pull a word onto another line.
  void readSyncClock() {
    _read_now = _rtc->getCurrentTime();
    _read_total_lines = -1;   // the ages are part of the text, so the wrapping may have moved
  }

  // back to the top, and hold there before scrolling starts. Arriving fresh on the page (or
  // switching conversation) earns another uninterrupted scroll through.
  void readRestart() {
    _read_scroll = 0;
    _read_cycles_left = 1;
    _read_next_scroll = millis() + UI_READ_TOP_MILLIS;
    _read_timing_set = true;
    readSyncClock();
  }

  // Throw away the current fetch, so the next render rebuilds it. Used when the conversation
  // being read changes, where the history itself hasn't moved but the selection has.
  void readInvalidate() { _read_fetched = false; }

  int targetRows() const { return _num_targets; }
  int sendMsgRows() const { return UI_SEND_NUM_MESSAGES; }

  // First row to draw in a picker window. The selection is kept one row up from the bottom
  // where there is room, so the entry coming next is already on screen, and the window never
  // runs past the end of the list into blank rows.
  int pickerWindowStart(int sel, int total, int rows) const {
    int lookahead = (rows > 2) ? 1 : 0;
    int first = sel - (rows - 1 - lookahead);
    if (first > total - rows) first = total - rows;
    if (first < 0) first = 0;
    return first;
  }

  // Default the READ target to the first subscribed channel. Deferred rather than done in the
  // constructor, because the mesh loads its channels from flash after the UI is constructed.
  void readEnsureTarget() {
    if (_read_target.name[0] != 0) return;

    ChannelDetails ch;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;   // unused slot

      memset(&_read_target, 0, sizeof(_read_target));
      _read_target.is_channel = true;
      _read_target.channel_idx = i;
      StrHelper::strncpy(_read_target.name, ch.name, sizeof(_read_target.name));
      // the channels arrive after the first few renders, so an earlier fetch will have come
      // back empty against no target at all -- it has to be redone now there is one
      readInvalidate();
      return;
    }
    // no channels configured at all -- leave the target unset, the page shows "No messages"
  }

  // Rebuild the list of messages to show, but only when there is a reason to. The history
  // version only moves when a message is added, so a page re-rendering once a second while
  // scrolling normally does no work here at all.
  void readFetch() {
    readEnsureTarget();

    uint32_t version = the_mesh.getMsgHistoryVersion();
    if (_read_fetched && version == _read_version) return;
    bool arrived = _read_fetched;   // a change, rather than the first look at this conversation
    _read_version = version;
    _read_fetched = true;
    if (!arrived) readSyncClock();   // first sight of it, so there is no pass to disturb

    if (_read_target.name[0] == 0) {
      _read_num = 0;
    } else if (_read_target.is_channel) {
      _read_num = the_mesh.getChannelHistory(_read_target.channel_idx, _read_idx,
                                             UI_READ_MSG_COUNT);
    } else {
      _read_num = the_mesh.getContactHistory(_read_target.pubkey_prefix,
                                             sizeof(_read_target.pubkey_prefix),
                                             _read_idx, UI_READ_MSG_COUNT);
    }

    _read_total_lines = -1;   // different messages, so the line count has to be redone

    // Looking at a conversation is what marks it read, so do it on every rebuild -- both the
    // first sight of it and each message that lands while it is on screen.
    readMarkSeen();

    // Messages come out newest-first, so one arriving is inserted at the top and pushes
    // everything below it down the screen. Left alone, the text the user was part-way through
    // reading would slide out from under the scroll position; going back to the top instead
    // keeps the view honest and shows what just came in.
    if (arrived) readRestart();
  }

  // Find the mark for a conversation, or NULL. Channels and contacts never collide: a mark is
  // one or the other, and is only compared against its own kind.
  ReadMark* readFindMark(const PickTarget& t) {
    for (int i = 0; i < UI_READ_MARK_COUNT; i++) {
      auto m = &_read_marks[i];
      if (!m->used || m->is_channel != t.is_channel) continue;
      if (t.is_channel) {
        if (m->channel_idx == t.channel_idx) return m;
      } else if (memcmp(m->pubkey_prefix, t.pubkey_prefix, sizeof(m->pubkey_prefix)) == 0) {
        return m;
      }
    }
    return NULL;
  }

  // Record that the conversation now being read has been seen up to its newest message, so the
  // picker stops marking it. Recycles the stalest mark when the table is full.
  void readMarkSeen() {
    if (_read_target.name[0] == 0) return;

    uint32_t newest = the_mesh.getNewestMsgTime(_read_target.is_channel,
                                                _read_target.channel_idx,
                                                _read_target.pubkey_prefix,
                                                sizeof(_read_target.pubkey_prefix));
    if (newest == 0) return;   // nothing in the ring for it

    auto mark = readFindMark(_read_target);
    if (mark == NULL) {
      mark = &_read_marks[0];
      for (int i = 1; i < UI_READ_MARK_COUNT; i++) {
        if (!_read_marks[i].used) { mark = &_read_marks[i]; break; }
        if (_read_marks[i].seen < mark->seen) mark = &_read_marks[i];
      }
      memset(mark, 0, sizeof(*mark));
      mark->used = true;
      mark->is_channel = _read_target.is_channel;
      mark->channel_idx = _read_target.channel_idx;
      memcpy(mark->pubkey_prefix, _read_target.pubkey_prefix, sizeof(mark->pubkey_prefix));
    }
    mark->seen = newest;
  }

  // Has this conversation had a message since it was last read? A conversation never opened
  // counts as unread, so long as it has something in it to read.
  bool readHasUnread(const PickTarget& t) {
    uint32_t newest = the_mesh.getNewestMsgTime(t.is_channel, t.channel_idx, t.pubkey_prefix,
                                                sizeof(t.pubkey_prefix));
    if (newest == 0) return false;

    auto mark = readFindMark(t);
    return mark == NULL || newest > mark->seen;
  }

  // Length of the longest prefix of str that fits in max_width, broken at a space where one is
  // available so words are not split. Always returns >= 1 so the caller cannot loop forever.
  // Measures in place, putting the terminator back afterwards, rather than copying each prefix
  // into a scratch buffer: that buffer used to cap a line at its own length regardless of how
  // much the display could actually fit, which showed up as early wrapping on the wider TFTs.
  int readWrapPoint(DisplayDriver& display, char* str, int max_width) {
    int fits = 0, last_space = 0;
    for (int n = 1; str[n - 1] != 0; n++) {
      char saved = str[n];
      str[n] = 0;
      bool too_wide = ((int)display.getTextWidth(str) > max_width);
      str[n] = saved;
      if (too_wide) break;
      fits = n;
      if (str[n - 1] == ' ') last_space = n;
    }
    if (str[fits] != 0 && last_space > 0) return last_space;   // would split a word
    return fits > 0 ? fits : 1;
  }

  // How long ago a message arrived, in the same shorthand the unread preview screen uses.
  // Measured against the pinned clock, not the live one -- see readSyncClock().
  void readFormatAge(char* dest, size_t dest_size, uint32_t recv_timestamp) {
    // The clock can be stepped backwards by a time sync, leaving a message "in the future"
    uint32_t secs = (_read_now > recv_timestamp) ? (_read_now - recv_timestamp) : 0;

    if (secs < 60) {
      snprintf(dest, dest_size, "%us", (unsigned) secs);
    } else if (secs < 60*60) {
      snprintf(dest, dest_size, "%um", (unsigned) (secs / 60));
    } else if (secs < 24*60*60) {
      snprintf(dest, dest_size, "%uh", (unsigned) (secs / (60*60)));
    } else {
      snprintf(dest, dest_size, "%ud", (unsigned) (secs / (24*60*60)));
    }
  }

  // Width available to text. The scrollbar gutter is reserved whether or not the bar is
  // actually drawn, so that the wrapped line count doesn't change as it appears/disappears.
  int readTextWidth(DisplayDriver& display) const {
    return display.width() - UI_READ_SCROLLBAR_W - 1;
  }

  // Thin scrollbar down the right edge. Only the thumb is drawn: its length is the fraction of
  // the conversation on screen, its position is how far down that window sits. Deliberately no
  // background track -- every display driver here defines primary_txt and secondary_txt as the
  // same colour, so a track would merge with the thumb into one featureless bar.
  void readDrawScrollbar(DisplayDriver& display, int scroll_px, int total_px) {
    int h = display.height();
    int x = display.width() - UI_READ_SCROLLBAR_W;

    int thumb = (h * h) / total_px;
    if (thumb < 3) thumb = 3;          // keep it visible on very long conversations
    if (thumb > h) thumb = h;

    int max_scroll = total_px - h;
    int y = (max_scroll > 0) ? ((h - thumb) * scroll_px) / max_scroll : 0;

    display.setColor(UIColor::primary_txt);
    display.fillRect(x, y, UI_READ_SCROLLBAR_W, thumb);
  }

  // Draw the wrapped message lines, shifted up by scroll_px, and return the total line count.
  // Pass draw = false to count the lines without rendering. Scrolling is by pixels, so the
  // line at the top is usually part-way off screen; the display driver clips it.
  int readDrawLines(DisplayDriver& display, int scroll_px, bool draw) {
    // room for the longest message, the sender, and the age prefix in front of both
    char composed[sizeof(MsgHistoryEntry::sender) + sizeof(MsgHistoryEntry::text) + 16];
    char shown[sizeof(composed)];
    char age[12];
    int line = 0;

    for (int m = 0; m < _read_num; m++) {
      auto msg = the_mesh.getMsgHistoryEntry(_read_idx[m]);
      if (msg == NULL) continue;

      readFormatAge(age, sizeof(age), msg->recv_timestamp);
      // channel payloads already lead with "<sender>: "; DMs need the contact name prepended
      if (msg->sender[0] != 0) {
        snprintf(composed, sizeof(composed), "%s %s: %s", age, msg->sender, msg->text);
      } else {
        snprintf(composed, sizeof(composed), "%s %s", age, msg->text);
      }

      // A message can carry newlines, and translateUTF8ToBlocks drops anything outside printable
      // ASCII -- which used to run the words either side of a break together. Wrap each line of
      // the message separately instead, so the break is kept as a break.
      for (char* seg = composed; seg != NULL; ) {
        char* brk = strchr(seg, '\n');
        if (brk != NULL) *brk = 0;
        display.translateUTF8ToBlocks(shown, seg, sizeof(shown));
        seg = (brk != NULL) ? brk + 1 : NULL;

        for (char* p = shown; ; ) {
          if (*p == 0) { line++; break; }   // a blank line in the message still takes a row

          int n = readWrapPoint(display, p, readTextWidth(display));
          int y = line * UI_READ_LINE_HEIGHT - scroll_px;
          if (draw && y > -UI_READ_LINE_HEIGHT && y < display.height()) {
            char saved = p[n];
            p[n] = 0;
            display.setColor(UIColor::primary_txt);
            display.setCursor(0, y);
            display.print(p);
            p[n] = saved;
          }
          line++;
          p += n;
          while (*p == ' ') p++;   // don't start the next line on the break space
          if (*p == 0) break;
        }
      }
    }
    return line;
  }

  // send the picked message to whichever conversation is currently being read
  void sendPickedMessage() {
    auto target = &_read_target;
    const char* text = UI_SEND_MESSAGES[_read_msg_sel];
    char alert[56];
    bool sent, missing;

    _task->notify(UIEventType::ack);
    if (target->is_channel) {
      int rc = the_mesh.sendTextToChannelIdx(target->channel_idx, text);
      sent = (rc == MyMesh::CHANNEL_TXT_OK);
      missing = (rc == MyMesh::CHANNEL_TXT_NO_CHANNEL);
    } else {
      int rc = the_mesh.sendTextToNode(target->pubkey_prefix, sizeof(target->pubkey_prefix), text);
      sent = (rc == MyMesh::NODE_TXT_OK);
      missing = (rc == MyMesh::NODE_TXT_NO_CONTACT);
    }

    if (sent) {
      snprintf(alert, sizeof(alert), "%s -> %s", text, target->name);
      _task->showAlert(alert, 1500);
    } else if (missing) {
      // the target was picked some time ago, so it can have gone away since
      _task->showAlert(target->is_channel ? "Channel is gone" : "Not a contact", 1500);
    } else {
      snprintf(alert, sizeof(alert), "%s failed..", text);
      _task->showAlert(alert, 1000);
    }
  }

  // one row of a picker list: '>' marker on the selection, name ellipsized to fit, and an
  // optional '*' after it for a conversation with something unread in it
  void drawPickerRow(DisplayDriver& display, int y, bool is_sel, const char* text,
                     bool unread = false) {
    display.setColor(is_sel ? UIColor::warning_txt : UIColor::secondary_txt);
    display.setCursor(0, y);
    display.print(is_sel ? ">" : " ");

    int right = display.width();
    if (unread) {
      int w = display.getTextWidth("*");
      display.setCursor(right - w, y);
      display.print("*");
      right -= w + 2;
    }
    display.drawTextEllipsized(8, y, right - 8, text);
  }

  // Do two picker entries name the same conversation?
  bool targetsMatch(const PickTarget& a, const PickTarget& b) const {
    if (a.is_channel != b.is_channel) return false;
    if (a.is_channel) return a.channel_idx == b.channel_idx;
    return memcmp(a.pubkey_prefix, b.pubkey_prefix, sizeof(a.pubkey_prefix)) == 0;
  }

  bool targetAlreadyListed(const uint8_t* pub_key) const {
    for (int i = 0; i < _num_targets; i++) {
      if (_targets[i].is_channel) continue;
      if (memcmp(_targets[i].pubkey_prefix, pub_key,
                 sizeof(_targets[i].pubkey_prefix)) == 0) return true;
    }
    return false;
  }

  PickTarget* newTarget() {
    auto dest = &_targets[_num_targets++];
    memset(dest, 0, sizeof(*dest));
    return dest;
  }

  // every configured (subscribed) channel, at the top of the list
  void appendChannelTargets() {
    ChannelDetails ch;
    int added = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS && added < UI_TARGET_CHANNEL_MAX
                    && _num_targets < UI_TARGET_LIST_SIZE; i++) {
      if (!the_mesh.getChannel(i, ch) || ch.name[0] == 0) continue;   // unused slot

      auto dest = newTarget();
      dest->is_channel = true;
      dest->channel_idx = i;
      StrHelper::strncpy(dest->name, ch.name, sizeof(dest->name));
      added++;
    }
  }

  void appendRecentTargets() {
    int n = the_mesh.getRecentlyHeard(_recent_scratch, UI_TARGET_RECENT_MAX);
    for (int i = 0; i < n && _num_targets < UI_TARGET_LIST_SIZE; i++) {
      if (_recent_scratch[i].name[0] == 0) continue;   // empty slot
      if (_recent_scratch[i].type == ADV_TYPE_REPEATER) continue;   // nothing to say to one

      auto dest = newTarget();
      memcpy(dest->pubkey_prefix, _recent_scratch[i].pubkey_prefix, sizeof(dest->pubkey_prefix));
      StrHelper::strncpy(dest->name, _recent_scratch[i].name, sizeof(dest->name));
    }
  }

  // Favourites are worth being able to reach even when they haven't been heard from lately,
  // so append any the recently-heard list didn't already cover. Repeaters are skipped even
  // when favourited: they are infrastructure, not something to send a message to.
  void appendFavouriteTargets() {
    ContactInfo contact;
    auto iter = the_mesh.startContactsIterator();
    while (_num_targets < UI_TARGET_LIST_SIZE && iter.hasNext(&the_mesh, contact)) {
      if ((contact.flags & CONTACT_FLAG_FAVOURITE) == 0) continue;
      if (contact.type == ADV_TYPE_REPEATER) continue;
      if (contact.name[0] == 0 || targetAlreadyListed(contact.id.pub_key)) continue;

      auto dest = newTarget();
      memcpy(dest->pubkey_prefix, contact.id.pub_key, sizeof(dest->pubkey_prefix));
      StrHelper::strncpy(dest->name, contact.name, sizeof(dest->name));
    }
  }

  // Snapshot what can be picked: channels first, then recently heard, then favourites. Shared
  // by the YO and READ pickers. Taking a copy matters: getRecentlyHeard() re-sorts the live
  // table on every call, so reading it again between "select" and "use" could shift the list
  // under the user and act on the wrong one.
  void takeTargetSnapshot() {
    _num_targets = 0;
    appendChannelTargets();
    appendRecentTargets();
    appendFavouriteTargets();
  }


  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
    const int minMilliVolts = BATT_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATT_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(UIColor::title_txt);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);

    // show muted icon if buzzer is muted
#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      display.setColor(UIColor::warning_txt);
      display.drawXbm(iconX - 9, iconY + 1, muted_icon, 8, 8);
    }
#endif
  }

  CayenneLPP sensors_lpp;
  int sensors_nb = 0;
  bool sensors_scroll = false;
  int sensors_scroll_offset = 0;
  int next_sensors_refresh = 0;

  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      sensors.querySensors(0xFF, sensors_lpp);
      LPPReader reader (sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb ++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
#if AUTO_OFF_MILLIS > 0
      next_sensors_refresh = millis() + 5000; // refresh sensor values every 5 sec
#else
      next_sensors_refresh = millis() + 60000; // refresh sensor values every 1 min
#endif
    }
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _shutdown_init(false), _num_targets(0), _read_stage(READ_VIEW), _read_sel(0),
       _read_msg_sel(0), _read_pick_expiry(0), _read_scroll(0), _read_next_scroll(0),
       _read_timing_set(false), _read_scrolling(false), _read_cycles_left(1), _read_num(0),
       _read_version(0), _read_fetched(false), _read_now(0), _read_total_lines(-1),
       sensors_lpp(200) {
    memset(&_read_target, 0, sizeof(_read_target));
    memset(_read_marks, 0, sizeof(_read_marks));
  }

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
    // With multi-click off there is no back gesture inside the pickers, and long-press in the
    // message picker sends -- so idling out is the way to leave without acting.
    if (_read_stage != READ_VIEW && uiTimePassed(_read_pick_expiry)) {
      _read_stage = READ_VIEW;
    }
  }

  // the pickers are long lists, and are stepped through with repeated clicks
  bool wantsFastClicks() const override {
    return _page == HomePage::READ && _read_stage != READ_VIEW;
  }

  // Hold the display on while the conversation is part-way through a scroll it still owes the
  // user. The counter matters: without it a fresh cycle would start holding again seconds
  // after the last one ended, and the display would never blank at all. The hold covers the
  // pause at the top as well as the scroll itself -- keying it off the scroll position instead
  // let the display blank during that first pause, before a word of the page had moved.
  bool preventsSleep() const override {
    return _page == HomePage::READ && _read_stage == READ_VIEW
           && _read_scrolling && _read_cycles_left > 0;
  }

  // The READ page is already a view of the messages, so having one arriving throw a preview
  // over the top of it is noise: the conversation picks the message up by itself and jumps
  // back to the top to show it. The pickers count too -- losing the screen part-way through
  // choosing what to read, or what to send, is the same interruption.
  bool suppressesMsgPreview() const override {
    return _page == HomePage::READ;
  }

  // Woken from the auto-off blank. Waking part-way down a conversation means the rest of that
  // pass was missed, so cover what remains of it plus one more pass from the top; waking at
  // the top only needs the usual single pass.
  void onDisplayWake() override {
    if (_page != HomePage::READ || _read_stage != READ_VIEW) return;
    _read_cycles_left = (_read_scroll > 0) ? 2 : 1;
  }

  int render(DisplayDriver& display) override {
    char tmp[80];
    // The message view gets the whole display: the node name and battery are on every other
    // page, so repeating them here would only cost rows of text.
    bool show_title_bar = !(_page == HomePage::READ && _read_stage == READ_VIEW);

    if (show_title_bar) {
      display.setColor(UIColor::title_bkg);
      display.fillRect(0, 0, display.width(), 12);

      // node name
      display.setTextSize(1);
      display.setColor(UIColor::title_txt);
      char filtered_name[sizeof(_node_prefs->node_name)];
      display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name,
                                    sizeof(filtered_name));
      display.setCursor(0, 2);
      display.print(filtered_name);

      // battery voltage
      renderBatteryIndicator(display, _task->getBattMilliVolts());

      // curr page indicator
      if (UIColor::title_bkg == UIColor::window_bkg) {
        display.setColor(UIColor::title_txt);
      } else {
        display.setColor(UIColor::title_bkg);
      }
      int y = 14;
      int x = display.width() / 2 - 5 * (HomePage::Count-1);
      for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
        if (i == _page) {
          display.fillRect(x-1, y-1, 4, 4);
        } else {
          display.fillRect(x, y, 2, 2);
        }
      }
    }

    if (_page == HomePage::STATUS) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d", _task->getMsgCount());
      display.drawTextCentered(display.width() / 2, 22, tmp);

      #ifdef WIFI_SSID
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 54, tmp);
      #endif
      if (_task->hasConnection()) {
        display.setColor(UIColor::warning_txt);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 43, "< Connected >");

      } else if (the_mesh.getBLEPin() != 0) { // BT pin
        display.setColor(UIColor::warning_txt);
        display.setTextSize(2);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        display.drawTextCentered(display.width() / 2, 43, tmp);
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(UIColor::primary_txt);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 60) {
          sprintf(tmp, "%ds", secs);
        } else if (secs < 60*60) {
          sprintf(tmp, "%dm", secs / 60);
        } else {
          sprintf(tmp, "%dh", secs / (60*60));
        }

        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;

        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, 42);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
    } else if (_page == HomePage::BLUETOOTH) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18,
          _task->isBluetoothEnabled() ? bluetooth_on : bluetooth_off,
          32, 32);
      display.setColor(UIColor::secondary_txt);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 64 - 11, "toggle: " PRESS_LABEL);
    } else if (_page == HomePage::ADVERT) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
    } else if (_page == HomePage::READ) {
      _read_scrolling = false;
      char label[sizeof(_targets[0].name)];
      if (_read_stage == READ_PICK_TARGET) {
        int first = pickerWindowStart(_read_sel, targetRows(), UI_PICK_VISIBLE_ROWS);

        int y = 20;
        for (int i = first; i < targetRows() && i < first + UI_PICK_VISIBLE_ROWS; i++, y += 11) {
          display.translateUTF8ToBlocks(label, _targets[i].name, sizeof(label));
          drawPickerRow(display, y, i == _read_sel, label, readHasUnread(_targets[i]));
        }
      } else if (_read_stage == READ_PICK_MSG) {
        int first = pickerWindowStart(_read_msg_sel, sendMsgRows(), UI_PICK_VISIBLE_ROWS);

        int y = 20;
        for (int i = first; i < sendMsgRows() && i < first + UI_PICK_VISIBLE_ROWS; i++, y += 11) {
          drawPickerRow(display, y, i == _read_msg_sel, UI_SEND_MESSAGES[i]);
        }
      } else {
        readFetch();
        if (_read_num == 0) {
          display.setColor(UIColor::secondary_txt);
          display.drawTextCentered(display.width() / 2, display.height() / 2 - 5,
                                   "No messages");
        } else {
          // Measure the text first, so scrolling can stop once the last line sits on the
          // bottom of the screen -- going further would just pull blank space into view. The
          // count is cached, since re-wrapping every message is the expensive part of a render
          // and nothing about the text changes between the events that clear it.
          if (_read_total_lines < 0) _read_total_lines = readDrawLines(display, 0, false);
          int total_px = _read_total_lines * UI_READ_LINE_HEIGHT;
          int max_scroll = total_px - display.height();
          if (max_scroll < 0) max_scroll = 0;   // it all fits: nothing to scroll

          // first render since boot: hold at the top for a full interval before scrolling
          if (!_read_timing_set) {
            _read_next_scroll = millis() + readHoldMillis(max_scroll);
            _read_timing_set = true;
          }

          _read_scrolling = (max_scroll > 0);
          if (uiTimePassed(_read_next_scroll)) {
            if (_read_scroll >= max_scroll) {
              // Back at the top of the conversation, so the ages are free to move on without
              // disturbing anything. A conversation short enough not to scroll sits here
              // permanently, and gets its ages refreshed once per hold instead.
              _read_scroll = 0;                  // wrap round to the newest message
              readSyncClock();
              if (_read_scrolling && _read_cycles_left > 0) {
                _read_cycles_left--;             // that pass is now paid off
              }
            } else {
              _read_scroll += UI_READ_SCROLL_STEP;
              if (_read_scroll > max_scroll) _read_scroll = max_scroll;   // land on the end
            }
            // readHoldMillis() reads the new position, so landing on either end holds longer
            _read_next_scroll = millis() + readHoldMillis(max_scroll);
          }
          if (_read_scroll > max_scroll) _read_scroll = 0;   // messages arrived/aged out
          readDrawLines(display, _read_scroll, true);
          if (max_scroll > 0) readDrawScrollbar(display, _read_scroll, total_px);
        }
      }
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 18;
      bool gps_state = _task->getGPSState();
#ifdef PIN_GPS_SWITCH
      bool hw_gps_state = digitalRead(PIN_GPS_SWITCH);
      if (gps_state != hw_gps_state) {
        strcpy(buf, gps_state ? "gps off(hw)" : "gps off(sw)");
      } else {
        strcpy(buf, gps_state ? "gps on" : "gps off");
      }
#else
      strcpy(buf, gps_state ? "gps on" : "gps off");
#endif
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, buf);
      if (nmea == NULL) {
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else {
        display.setColor(UIColor::primary_txt);
        strcpy(buf, nmea->isValid()?"fix":"no fix");
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "sat");
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "%d", nmea->satellitesCount());
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "pos");
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "%.4f %.4f",
          nmea->getLatitude()/1000000., nmea->getLongitude()/1000000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.setColor(UIColor::secondary_txt);
        display.drawTextLeftAlign(0, y, "alt");
        display.setColor(UIColor::primary_txt);
        sprintf(buf, "%.2f", nmea->getAltitude()/1000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
      }
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = 18;
      refresh_sensors();
      char buf[30];
      char name[30];
      LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

      for (int i = 0; i < sensors_scroll_offset; i++) {
        uint8_t channel, type;
        r.readHeader(channel, type);
        r.skipData(type);
      }

      for (int i = 0; i < (sensors_scroll?UI_RECENT_LIST_SIZE:sensors_nb); i++) {
        uint8_t channel, type;
        if (!r.readHeader(channel, type)) { // reached end, reset
          r.reset();
          r.readHeader(channel, type);
        }

        display.setCursor(0, y);
        float v;
        switch (type) {
          case LPP_GPS: // GPS
            float lat, lon, alt;
            r.readGPS(lat, lon, alt);
            strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
            break;
          case LPP_VOLTAGE:
            r.readVoltage(v);
            strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
            break;
          case LPP_CURRENT:
            r.readCurrent(v);
            strcpy(name, "current"); sprintf(buf, "%.3f", v);
            break;
          case LPP_TEMPERATURE:
            r.readTemperature(v);
            strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
            break;
          case LPP_RELATIVE_HUMIDITY:
            r.readRelativeHumidity(v);
            strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
            break;
          case LPP_BAROMETRIC_PRESSURE:
            r.readPressure(v);
            strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
            break;
          case LPP_ALTITUDE:
            r.readAltitude(v);
            strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
            break;
          case LPP_POWER:
            r.readPower(v);
            strcpy(name, "power"); sprintf(buf, "%6.2f", v);
            break;
          default:
            r.skipData(type);
            strcpy(name, "unk"); sprintf(buf, "");
        }
        display.setCursor(0, y);
        display.setColor(UIColor::secondary_txt);
        display.print(name);
        display.setColor(UIColor::primary_txt);
        display.setCursor(
          display.width()-display.getTextWidth(buf)-1, y
        );
        display.print(buf);
        y = y + 12;
      }
      if (sensors_scroll) sensors_scroll_offset = (sensors_scroll_offset+1)%sensors_nb;
      else sensors_scroll_offset = 0;
#endif
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(UIColor::corp_blue);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.setColor(UIColor::warning_txt);
        display.drawTextCentered(display.width() / 2, 34, "hibernating...");
      } else {
        display.setColor(UIColor::secondary_txt);
        display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
        display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate:" PRESS_LABEL);
      }
    }
    // the READ page drives its own scrolling, so it needs re-rendering on that interval
    if (_read_scrolling) return UI_READ_SCROLL_MILLIS;
    return 5000;   // next render after 5000 ms
  }

  bool handleInput(char c) override {
    // the pickers are modal -- they swallow every key, so that a short press steps through
    // the list instead of paging the carousel
    if (_read_stage == READ_PICK_TARGET) {
      _read_pick_expiry = millis() + UI_PICK_TIMEOUT_MILLIS;
      if (c == KEY_NEXT || c == KEY_RIGHT) {   // short press -> next row
        _read_sel = (_read_sel + 1) % targetRows();
      } else if (c == KEY_PREV || c == KEY_LEFT) {   // double-tap -> back, without picking
        _read_stage = READ_VIEW;
      } else if (c == KEY_ENTER) {   // long press -> read the selected conversation
        _read_target = _targets[_read_sel];
        readInvalidate();   // different conversation, so the fetch has to be redone
        readRestart();
        // the message view has no title bar, so confirm the switch here instead
        char alert[48];
        snprintf(alert, sizeof(alert), "Reading: %s", _read_target.name);
        _task->showAlert(alert, 1200);
        _read_stage = READ_VIEW;
      }
      return true;
    }
    if (_read_stage == READ_PICK_MSG) {
      _read_pick_expiry = millis() + UI_PICK_TIMEOUT_MILLIS;
      if (c == KEY_NEXT || c == KEY_RIGHT) {   // short press -> next row
        _read_msg_sel = (_read_msg_sel + 1) % sendMsgRows();
      } else if (c == KEY_PREV || c == KEY_LEFT) {   // double-tap -> back, without sending
        _read_stage = READ_VIEW;
      } else if (c == KEY_ENTER) {   // long press -> send it
        sendPickedMessage();
        _read_stage = READ_VIEW;
      }
      return true;
    }
    // double-tap while reading -> pick a message to send to this conversation. Checked ahead
    // of the carousel navigation below, which is what KEY_PREV normally does.
    if ((c == KEY_PREV || c == KEY_LEFT) && _page == HomePage::READ) {
      readEnsureTarget();
      if (_read_target.name[0] == 0) {
        _task->showAlert("Nothing to send to", 1200);
      } else {
        _read_msg_sel = 0;
        _read_pick_expiry = millis() + UI_PICK_TIMEOUT_MILLIS;
        _read_stage = READ_PICK_MSG;
      }
      return true;
    }
    if (c == KEY_LEFT || c == KEY_PREV) {
      _page = (_page + HomePage::Count - 1) % HomePage::Count;
      if (_page == HomePage::READ) readRestart();
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _page = (_page + 1) % HomePage::Count;
      if (_page == HomePage::RECENT) {
        _task->showAlert("Recent adverts", 800);
      } else if (_page == HomePage::READ) {
        readRestart();   // always arrive at the top of the conversation
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::BLUETOOTH) {
      if (_task->isBluetoothEnabled()) {  // toggle Bluetooth on/off
        _task->disableBluetooth();
      } else {
        _task->enableBluetooth();
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::ADVERT) {
      _task->notify(UIEventType::ack);
      if (the_mesh.advert()) {
        _task->showAlert("Advert sent!", 1000);
      } else {
        _task->showAlert("Advert failed..", 1000);
      }
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::READ) {   // long press -> pick what to read
      takeTargetSnapshot();
      if (_num_targets == 0) {
        _task->showAlert("Nothing to read", 1200);
      } else {
        // open on the conversation being read, so stepping off it is a deliberate act rather
        // than something the user has to walk back to the top of the list to undo
        _read_sel = 0;
        for (int i = 0; i < _num_targets; i++) {
          if (targetsMatch(_targets[i], _read_target)) { _read_sel = i; break; }
        }
        _read_pick_expiry = millis() + UI_PICK_TIMEOUT_MILLIS;
        _read_stage = READ_PICK_TARGET;
      }
      return true;
    }
#if ENV_INCLUDE_GPS == 1
    if (c == KEY_ENTER && _page == HomePage::GPS) {
      _task->toggleGPS();
      return true;
    }
#endif
#if UI_SENSORS_PAGE == 1
    if (c == KEY_ENTER && _page == HomePage::SENSORS) {
      _task->toggleGPS();
      next_sensors_refresh=0;
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::SHUTDOWN) {
      _shutdown_init = true;  // need to wait for button to be released
      return true;
    }
    return false;
  }
};

class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  struct MsgEntry {
    uint32_t timestamp;
    char origin[62];
    char msg[78];
  };
  #define MAX_UNREAD_MSGS   32
  int num_unread;
  int head = MAX_UNREAD_MSGS - 1; // index of latest unread message
  MsgEntry unread[MAX_UNREAD_MSGS];

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    head = (head + 1) % MAX_UNREAD_MSGS;
    if (num_unread < MAX_UNREAD_MSGS) num_unread++;

    auto p = &unread[head];
    p->timestamp = _rtc->getCurrentTime();
    if (path_len == 0xFF) {
      sprintf(p->origin, "(D) %s:", from_name);
    } else {
      sprintf(p->origin, "(%d) %s:", (uint32_t) path_len, from_name);
    }
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
  }

  int render(DisplayDriver& display) override {
    char tmp[16];
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setColor(UIColor::corp_blue);
    sprintf(tmp, "Unread: %d", num_unread);
    display.print(tmp);

    auto p = &unread[head];

    int secs = _rtc->getCurrentTime() - p->timestamp;
    if (secs < 60) {
      sprintf(tmp, "%ds", secs);
    } else if (secs < 60*60) {
      sprintf(tmp, "%dm", secs / 60);
    } else {
      sprintf(tmp, "%dh", secs / (60*60));
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    display.drawRect(0, 11, display.width(), 1);  // horiz line

    display.setCursor(0, 14);
    display.setColor(UIColor::secondary_txt);
    char filtered_origin[sizeof(p->origin)];
    display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));
    display.print(filtered_origin);

    display.setCursor(0, 25);
    display.setColor(UIColor::primary_txt);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    display.printWordWrap(filtered_msg, display.width());

#if AUTO_OFF_MILLIS==0 // probably e-ink
    return 10000; // 10 s
#else
    return 1000;  // next render after 1000 ms
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      head = (head + MAX_UNREAD_MSGS - 1) % MAX_UNREAD_MSGS;
      num_unread--;
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      }
      return true;
    }
    if (c == KEY_ENTER) {
      num_unread = 0;  // clear unread queue
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + AUTO_OFF_MILLIS;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif

  _node_prefs = node_prefs;

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;

  splash = new SplashScreen(this);
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
  setCurrScreen(splash);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strcpy(_alert, text);
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}


void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  if (msgcount == 0) {
    gotoHomeScreen();
  }
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;

  // Queue it either way, so nothing is lost and the count stays right. Whether it also takes
  // over the display is up to whatever is currently showing.
  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from_name, text);
  if (curr == NULL || !curr->suppressesMsgPreview()) {
    setCurrScreen(msg_preview);
  }

  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
    if (_display->isOn()) {
    _auto_off = millis() + AUTO_OFF_MILLIS;  // extend the auto-off timer
    _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_msgcount > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  #ifdef PIN_BUZZER
  /* note: we have a choice here -
     we can do a blocking buzzer.loop() with non-deterministic consequences
     or we can set a flag and delay the shutdown for a couple of seconds
     while a non-blocking buzzer.loop() plays out in UITask::loop()
  */
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    // Power off board including radio, display, GPS and components
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::loop() {
  char c = 0;

  // Screens showing a long list opt out of multi-click detection, so that clicking quickly to
  // scan down the list reports each press on its own instead of coalescing into double-clicks.
  bool fast_clicks = (curr != NULL) && curr->wantsFastClicks();
#if defined(PIN_USER_BTN)
  user_btn.setMultiClick(!fast_clicks);
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.setMultiClick(!fast_clicks);
#endif

#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);  // REVISIT: could be mapped to different key code
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#endif
#if defined(UI_HAS_ROTARY_INPUT)
  RotaryInputEvent rotaryEv = rotary_input.poll();
  if (c == 0 && _display != NULL && _display->isOn()) {
    if (rotaryEv == RotaryInputEvent::Next) {
      c = KEY_NEXT;
    } else if (rotaryEv == RotaryInputEvent::Prev) {
      c = KEY_PREV;
    }
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    int ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#if defined(BACKLIGHT_BTN)
  if (millis() > next_backlight_btn_check) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 100;  // trigger refresh
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {  // render alert popup
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(UIColor::popup_bkg);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(UIColor::popup_txt);  // draw box border
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
      } else {
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
    }
#if AUTO_OFF_MILLIS > 0
#ifdef KEEP_DISPLAY_ON_USB
    // Opt-in: refresh the auto-off deadline while externally powered, so the
    // timer counts from the moment external power is removed. Off by default
    // because OLED panels burn in quickly; only enable for LCD targets or
    // where the display is replaceable.
    if (board.isExternalPowered()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
#endif
    // A screen part-way through something worth watching (the message view mid-scroll) holds
    // the deadline off, so the display doesn't blank halfway down a conversation.
    if (curr && curr->preventsSleep()) {
      _auto_off = millis() + AUTO_OFF_MILLIS;
    }
    if (millis() > _auto_off) {
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      if(!board.isExternalPowered()) {
        if (_display != NULL) {
          _display->startFrame();
          _display->setTextSize(2);
          _display->setColor(UIColor::warning_txt);
          _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
          _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
          _display->endFrame();
          if (_display->isEink() == false) { delay(3000); }
        }
        shutdown();
      }
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();   // turn display on and consume event
      c = 0;
      if (curr) curr->onDisplayWake();
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }
  return c;
}

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    c = 0;   // consume event
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double-click triggered");
  checkDisplayOn(c);
  return c;
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
  checkDisplayOn(c);
  toggleBuzzer();
  c = 0;
  return c;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  }
  return false;
}

void UITask::toggleGPS() {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        the_mesh.savePrefs();
        showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::toggleBuzzer() {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    the_mesh.savePrefs();
    showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    _next_refresh = 0;  // trigger refresh
  #endif
}
