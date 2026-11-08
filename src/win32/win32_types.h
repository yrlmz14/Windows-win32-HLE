#ifndef WIN32_TYPES_H
#define WIN32_TYPES_H

#include "../common.h"

/* Win32 Base Integral Types */
typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef int32_t   INT;
typedef uint32_t  UINT;
typedef int32_t   BOOL;
typedef void     *LPVOID;
typedef const void *LPCVOID;
typedef char     *LPSTR;
typedef const char *LPCSTR;
typedef uint16_t *LPWSTR;
typedef const uint16_t *LPCWSTR;

typedef uint32_t  WPARAM;
typedef int32_t   LPARAM;
typedef int32_t   LRESULT;

#define TRUE  1
#define FALSE 0

/* Guest Pointer type (all guest pointers are 32-bit linear addresses) */
typedef uint32_t  GUEST_PTR;

/* Opaque handles */
typedef uint32_t  H_HANDLE;
typedef H_HANDLE  H_HWND;
typedef H_HANDLE  H_HDC;
typedef H_HANDLE  H_HBITMAP;
typedef H_HANDLE  H_HFONT;
typedef H_HANDLE  H_HBRUSH;
typedef H_HANDLE  H_HPEN;
typedef H_HANDLE  H_HMENU;
typedef H_HANDLE  H_HINSTANCE;
typedef H_HANDLE  H_HICON;
typedef H_HANDLE  H_HCURSOR;
typedef H_HANDLE  H_HRGN;
typedef H_HANDLE  H_HKEY;

/* Point & Rect */
#pragma pack(push, 1)
typedef struct {
    int32_t x;
    int32_t y;
} WIN_POINT;

typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} WIN_RECT;

typedef struct {
    H_HDC     hdc;
    BOOL      fErase;
    WIN_RECT  rcPaint;
    BOOL      fRestore;
    BOOL      fIncUpdate;
    BYTE      rgbReserved[32];
} WIN_PAINTSTRUCT;

typedef struct {
    H_HWND    hwnd;
    UINT      message;
    WPARAM    wParam;
    LPARAM    lParam;
    DWORD     time;
    WIN_POINT pt;
} WIN_MSG;

typedef struct {
    UINT      style;
    GUEST_PTR lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    H_HINSTANCE hInstance;
    H_HICON   hIcon;
    H_HCURSOR hCursor;
    H_HBRUSH  hbrBackground;
    GUEST_PTR lpszMenuName;
    GUEST_PTR lpszClassName;
} WIN_WNDCLASS;

typedef struct {
    UINT      cbSize;
    UINT      style;
    GUEST_PTR lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    H_HINSTANCE hInstance;
    H_HICON   hIcon;
    H_HCURSOR hCursor;
    H_HBRUSH  hbrBackground;
    GUEST_PTR lpszMenuName;
    GUEST_PTR lpszClassName;
    H_HICON   hIconSm;
} WIN_WNDCLASSEX;

typedef struct {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} WIN_BITMAPINFOHEADER;

typedef struct {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} WIN_RGBQUAD;

typedef struct {
    WIN_BITMAPINFOHEADER bmiHeader;
    WIN_RGBQUAD          bmiColors[256];
} WIN_BITMAPINFO;

typedef struct {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
    BYTE tmFirstChar;
    BYTE tmLastChar;
    BYTE tmDefaultChar;
    BYTE tmBreakChar;
    BYTE tmItalic;
    BYTE tmUnderlined;
    BYTE tmStruckOut;
    BYTE tmPitchAndFamily;
    BYTE tmCharSet;
} WIN_TEXTMETRIC;

#pragma pack(pop)

/* Common Win32 Window Messages */
#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_SETREDRAW        0x000B
#define WM_SETTEXT          0x000C
#define WM_GETTEXT          0x000D
#define WM_GETTEXTLENGTH    0x000E
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUERYENDSESSION  0x0011
#define WM_QUIT             0x0012
#define WM_ERASEBKGND       0x0014
#define WM_SHOWWINDOW       0x0018
#define WM_COMMAND          0x0111
#define WM_SYSCOMMAND       0x0112
#define WM_TIMER            0x0113
#define WM_HSCROLL          0x0114
#define WM_VSCROLL          0x0115
#define WM_INITDIALOG       0x0110

/* Mouse Messages */
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_RBUTTONDBLCLK    0x0206

/* Keyboard Messages */
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102

/* Window Styles */
#define WS_OVERLAPPED       0x00000000L
#define WS_POPUP            0x80000000L
#define WS_CHILD            0x40000000L
#define WS_MINIMIZE         0x20000000L
#define WS_VISIBLE          0x10000000L
#define WS_DISABLED         0x08000000L
#define WS_CLIPSIBLINGS     0x04000000L
#define WS_CLIPCHILDREN     0x02000000L
#define WS_MAXIMIZE         0x01000000L
#define WS_CAPTION          0x00C00000L
#define WS_BORDER           0x00800000L
#define WS_DLGFRAME         0x00400000L
#define WS_VSCROLL          0x00200000L
#define WS_HSCROLL          0x00100000L
#define WS_SYSMENU          0x00080000L
#define WS_THICKFRAME       0x00040000L
#define WS_MINIMIZEBOX      0x00020000L
#define WS_MAXIMIZEBOX      0x00010000L

/* ShowWindow flags */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOWMINIMIZED    2
#define SW_MAXIMIZE         3
#define SW_SHOW             5

/* Stock Objects */
#define WHITE_BRUSH         0
#define LTGRAY_BRUSH        1
#define GRAY_BRUSH          2
#define DKGRAY_BRUSH        3
#define BLACK_BRUSH         4
#define NULL_BRUSH          5
#define HOLLOW_BRUSH        NULL_BRUSH
#define WHITE_PEN           6
#define BLACK_PEN           7
#define NULL_PEN            8
#define OEM_FIXED_FONT      10
#define ANSI_FIXED_FONT     11
#define ANSI_VAR_FONT       12
#define SYSTEM_FONT         13
#define DEVICE_DEFAULT_FONT 14
#define DEFAULT_PALETTE     15
#define SYSTEM_FIXED_FONT   16

#endif /* WIN32_TYPES_H */
