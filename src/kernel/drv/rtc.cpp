// silicon - CMOS real time clock
#include "drivers.h"

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

static int day_of_week(int y, int m, int d) {   // 0 = Sunday
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void rtc_read(DateTime& dt) {
    while (cmos(0x0A) & 0x80) {}
    int s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04);
    int d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    u8 b = cmos(0x0B);
    if (!(b & 0x04)) {
        s = bcd(s); mi = bcd(mi); d = bcd(d); mo = bcd(mo); y = bcd(y);
        h = bcd(h & 0x7F) | (h & 0x80);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    dt.second = s; dt.minute = mi; dt.hour = h & 0x7F;
    dt.day = d; dt.month = mo; dt.year = 2000 + y;
    dt.weekday = day_of_week(dt.year, dt.month, dt.day);
}

// seconds since 2000-01-01 (months approximated by real month lengths)
static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

u32 rtc_stamp() {
    DateTime dt;
    rtc_read(dt);
    u32 days = 0;
    for (int y = 2000; y < dt.year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < dt.month; m++) days += mdays[m - 1] + (m == 2 && leap(dt.year));
    days += dt.day - 1;
    return days * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second;
}

void stamp_to_datetime(u32 st, DateTime& dt) {
    u32 days = st / 86400, rem = st % 86400;
    dt.hour = rem / 3600; dt.minute = rem % 3600 / 60; dt.second = rem % 60;
    int y = 2000;
    while (days >= (u32)(leap(y) ? 366 : 365)) { days -= leap(y) ? 366 : 365; y++; }
    int m = 1;
    while (true) {
        u32 md = mdays[m - 1] + (m == 2 && leap(y));
        if (days < md) break;
        days -= md; m++;
    }
    dt.year = y; dt.month = m; dt.day = days + 1;
    dt.weekday = day_of_week(y, m, dt.day);
}

const char* month_name(int m) {
    static const char* n[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return n[(m - 1) % 12];
}
const char* weekday_name(int d) {
    static const char* n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return n[d % 7];
}
