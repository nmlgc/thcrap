#include <thcrap.h>
#include <windows.h>
#include <windowsx.h>
#define ProgressBar_SetRange(hwndCtl, low, high) ((DWORD)SNDMSG((hwndCtl), PBM_SETRANGE, 0L, MAKELPARAM((low), (high))))
#define ProgressBar_SetPos(hwndCtl, pos) ((int)(DWORD)SNDMSG((hwndCtl), PBM_SETPOS, (WPARAM)(int)(pos), 0L))
#define ProgressBar_SetMarquee(hwndCtl, fEnable, animTime) ((BOOL)(DWORD)SNDMSG((hwndCtl), PBM_SETMARQUEE, (WPARAM)(BOOL)(fEnable), (LPARAM)(DWORD)(animTime)))
#include "console.h"
#include "dialog.h"
#include "resource.h"
#include <string.h>
#include <CommCtrl.h>
#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <type_traits>

// A wrapper around std::promise that lets us call set_value without
// having to worry about promise_already_satisfied/no_state.
template<typename R>
class PromiseSlot {
	static inline constexpr bool is_void = std::is_void<R>::value;
	static inline constexpr bool is_ref = std::is_lvalue_reference<R>::value;

	std::promise<R> promise;
	bool present;
public:
	PromiseSlot() {
		release();
	}
	PromiseSlot(std::promise<R> &&other) :promise(std::move(other)), present(true) {
	}
	PromiseSlot &operator=(std::promise<R> &&other) {
		std::promise<R>{std::move(other)}.swap(promise);
		present = true;
		return *this;
	}
	std::promise<R> release() {
		present = false;
		return std::promise<R>{std::move(promise)};
	}
	template<typename RR = std::enable_if_t<!is_void && !is_ref, R>>
	void set_value(const RR &value) {
		if (present)
			release().set_value(value);
	}
	template<typename = std::enable_if_t<is_void>>
	void set_value() {
		if (present)
			release().set_value();
	}
	explicit operator bool() {
		return present;
	}
};

struct ConsoleDialog : Dialog<ConsoleDialog> {
private:
	friend Dialog<ConsoleDialog>;
	HINSTANCE getInstance();
	LPCWSTR getTemplate();
	INT_PTR dialogProc(UINT uMsg, WPARAM wParam, LPARAM lParam);
private:
	HWND buttonNext = NULL;
	HWND edit = NULL;
	HWND list = NULL;
	HWND progress = NULL;
	HWND staticText = NULL;
	HWND buttonYes = NULL;
	HWND buttonNo = NULL;

	enum Mode {
		MODE_INPUT = 0,
		MODE_PAUSE,
		MODE_PROGRESS_BAR,
		MODE_NONE,
		MODE_ASK_YN,
	};
	int currentMode = -1;
	void setMode(Mode mode);

	static LRESULT CALLBACK editProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK listProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	void setMarquee(BOOL fEnable);

	PromiseSlot<char> onYesNo; // APP_ASKYN event
	PromiseSlot<wchar_t*> onInput; // APP_GETINPUT event
	PromiseSlot<void> onUnpause; // APP_PAUSE event

	enum {
		APP_READQUEUE = WM_APP,
		APP_ASKYN, // lparam = std::promise<char>*
		APP_GETINPUT, // lparam = std::promise<wchar_t*>*
		APP_LISTCHAR, // wparam = WM_CHAR
		APP_UPDATE,
		APP_PREUPDATE,
		APP_PAUSE, // lparam = std::promise<void>*
		APP_PROGRESS, // wparam = percent
	};
public:
	PromiseSlot<void> onInit;

	void readQueue();
	std::future<char> askyn();
	std::future<wchar_t*> getInput();
	void update();
	void preupdate();
	std::future<void> pause();
	void setProgress(int pc);
};
static ConsoleDialog g_console_xxx{};
static ConsoleDialog *g_console = &g_console_xxx;

void ConsoleDialog::readQueue() {
	PostMessage(hWnd, APP_READQUEUE, 0, 0L);
}
std::future<char> ConsoleDialog::askyn() {
	std::promise<char> promise;
	std::future<char> future = promise.get_future();
	SendMessage(hWnd, APP_ASKYN, 0, (LPARAM)&promise);
	return future;
}
std::future<wchar_t *> ConsoleDialog::getInput() {
	std::promise<wchar_t*> promise;
	std::future<wchar_t*> future = promise.get_future();
	SendMessage(hWnd, APP_GETINPUT, 0, (LPARAM)&promise);
	return future;
}
// Should be called after adding/appending bunch of lines
void ConsoleDialog::update() {
	PostMessage(hWnd, APP_UPDATE, 0, 0L);
}
// Should be called before adding lines
void ConsoleDialog::preupdate() {
	PostMessage(hWnd, APP_PREUPDATE, 0, 0L);
}
std::future<void> ConsoleDialog::pause() {
	std::promise<void> promise;
	std::future<void> future = promise.get_future();
	SendMessage(hWnd, APP_PAUSE, 0, (LPARAM)&promise);
	return future;
}
void ConsoleDialog::setProgress(int pc) {
	PostMessage(hWnd, APP_PROGRESS, (WPARAM)(DWORD)pc, 0L);
}

enum LineType {
	LINE_ADD,
	LINE_APPEND,
	LINE_CLS,
	LINE_PENDING
};
struct LineEntry {
	LineType type;
	std::wstring content;
};

static std::mutex g_mutex; // used for synchronizing the queue
static std::queue<LineEntry> g_queue;
static std::vector<std::wstring> q_responses;
static PromiseSlot<void> g_exitguithreadevent;
void ConsoleDialog::setMode(Mode mode) {
	if (currentMode == mode)
		return;
	currentMode = mode;
	static const HWND ConsoleDialog::*items[6] = {
		&ConsoleDialog::buttonNext,
		&ConsoleDialog::edit,
		&ConsoleDialog::progress,
		&ConsoleDialog::staticText,
		&ConsoleDialog::buttonYes,
		&ConsoleDialog::buttonNo
	};

	static const bool modes[5][6] = {
		{ true, true, false, false, false, false }, // MODE_INPUT
		{ true, false, false, false, false, false }, // MODE_PAUSE
		{ false, false, true, false, false, false }, // MODE_PROGRESS_BAR
		{ false, false, false, true, false, false }, // MODE_NONE
		{ false, false, false, false, true, true }, // MODE_ASK_YN
	};
	for (int i = 0; i < 6; i++) {
		if (modes[mode][i]) {
			EnableWindow(this->*items[i], TRUE);
			ShowWindow(this->*items[i], SW_SHOW);
		} else {
			ShowWindow(this->*items[i], SW_HIDE);
			EnableWindow(this->*items[i], FALSE);
		}
	}
}

static WNDPROC getClassWindowProc(HINSTANCE hInstance, LPCWSTR lpClassName) {
	WNDCLASS wndClass;
	if (!GetClassInfoW(hInstance, lpClassName, &wndClass))
		assert(0);
	return wndClass.lpfnWndProc;
}

LRESULT CALLBACK ConsoleDialog::editProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	ConsoleDialog *dlg = fromHandle(GetParent(hWnd));
	static WNDPROC origEditProc = NULL;
	if (!origEditProc)
		origEditProc = getClassWindowProc(NULL, WC_EDITW);

	if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
		SendMessage(dlg->hWnd, WM_COMMAND, MAKELONG(IDC_BUTTON1, BN_CLICKED), (LPARAM)hWnd);
		return 0;
	} else if (uMsg == WM_GETDLGCODE && wParam == VK_RETURN) {
		// nescessary for control to recieve VK_RETURN
		return CallWindowProcW(origEditProc, hWnd, uMsg, wParam, lParam) | DLGC_WANTALLKEYS;
	} else if (uMsg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) {
		// Switch focus to list on up/down arrows
		SetFocus(dlg->list);
		return 0;
	} else if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && (wParam == VK_PRIOR || wParam == VK_NEXT)) {
		// Switch focus to list on up/down arrows
		SendMessage(dlg->list, uMsg, wParam, lParam);
		return 0;
	}
	return CallWindowProcW(origEditProc, hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ConsoleDialog::listProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	ConsoleDialog *dlg = fromHandle(GetParent(hWnd));
	static WNDPROC origListProc = NULL;
	if (!origListProc)
		origListProc = getClassWindowProc(NULL, WC_LISTBOXW);

	if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
		// if edit already has something in it, clicking on the button
		// otherwise doubleclick the list
		int len = GetWindowTextLength(dlg->edit);
		SendMessage(dlg->hWnd, WM_COMMAND, len ? MAKELONG(IDC_BUTTON1, BN_CLICKED) : MAKELONG(IDC_LIST1, LBN_DBLCLK), (LPARAM)hWnd);
		return 0;
	} else if (uMsg == WM_GETDLGCODE && wParam == VK_RETURN) {
		// nescessary for control to recieve VK_RETURN
		return CallWindowProcW(origListProc, hWnd, uMsg, wParam, lParam) | DLGC_WANTALLKEYS;
	} else if (uMsg == WM_CHAR) {
		// let parent dialog process the keypresses from list
		SendMessage(dlg->hWnd, APP_LISTCHAR, wParam, lParam);
		return 0;
	}
	return CallWindowProcW(origListProc, hWnd, uMsg, wParam, lParam);
}

void ConsoleDialog::setMarquee(BOOL fEnable) {
	DWORD dwStyle = GetWindowLong(progress, GWL_STYLE);
	if (!!(dwStyle & PBS_MARQUEE) != fEnable) {
		if (fEnable)
			dwStyle |= PBS_MARQUEE;
		else
			dwStyle &= ~PBS_MARQUEE;
		SetWindowLong(progress, GWL_STYLE, dwStyle);
		ProgressBar_SetMarquee(progress, fEnable, 0L);
	}
}

bool con_can_close = false;
HINSTANCE ConsoleDialog::getInstance() {
	return GetModuleHandle(NULL);
}
LPCWSTR ConsoleDialog::getTemplate() {
	return MAKEINTRESOURCEW(IDD_DIALOG1);
}
INT_PTR ConsoleDialog::dialogProc(UINT uMsg, WPARAM wParam, LPARAM lParam) {
	static int last_index = 0;
	static std::wstring pending = L"";
	static HICON hIconSm = NULL, hIcon = NULL;
	switch (uMsg) {
	case WM_INITDIALOG: {
		buttonNext = GetDlgItem(hWnd, IDC_BUTTON1);
		edit = GetDlgItem(hWnd, IDC_EDIT1);
		list = GetDlgItem(hWnd, IDC_LIST1);
		progress = GetDlgItem(hWnd, IDC_PROGRESS1);
		staticText = GetDlgItem(hWnd, IDC_STATIC1);
		buttonYes = GetDlgItem(hWnd, IDC_BUTTON_YES);
		buttonNo = GetDlgItem(hWnd, IDC_BUTTON_NO);

		ProgressBar_SetRange(progress, 0, 100);
		SetWindowLongPtr(edit, GWLP_WNDPROC, (LONG_PTR)editProc);
		SetWindowLongPtr(list, GWLP_WNDPROC, (LONG_PTR)listProc);

		// set icon
		hIconSm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
		hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
		if (hIconSm) {
			SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
		}
		if (hIcon) {
			SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		}
		setMode(MODE_NONE);

		RECT workarea, winrect;
		SystemParametersInfo(SPI_GETWORKAREA, 0, (PVOID)&workarea, 0);
		GetWindowRect(hWnd, &winrect);
		int width = winrect.right - winrect.left;
		MoveWindow(hWnd,
			workarea.left + ((workarea.right - workarea.left) / 2) - (width / 2),
			workarea.top,
			width,
			workarea.bottom - workarea.top,
			TRUE);

		onInit.set_value();
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_BUTTON1:
			if (currentMode == MODE_INPUT) {
				int input_len = GetWindowTextLengthW(edit) + 1;
				wchar_t* input_str = new wchar_t[input_len];
				GetWindowTextW(edit, input_str, input_len);
				SetWindowTextW(edit, L"");
				setMode(MODE_NONE);
				onInput.set_value(input_str);
			}
			else if (currentMode == MODE_PAUSE) {
				setMode(MODE_NONE);
				onUnpause.set_value();
			}
			return TRUE;
		case IDC_BUTTON_YES:
		case IDC_BUTTON_NO:
			if (currentMode == MODE_ASK_YN) {
				setMode(MODE_NONE);
				onYesNo.set_value(LOWORD(wParam) == IDC_BUTTON_YES ? 'y' : 'n');
			}
			return TRUE;
		case IDC_LIST1:
			switch (HIWORD(wParam)) {
			case LBN_DBLCLK: {
				int cur = ListBox_GetCurSel((HWND)lParam);
				if (currentMode == MODE_INPUT && cur != LB_ERR && (!q_responses[cur].empty() || cur == last_index)) {
					wchar_t* input_str = new wchar_t[q_responses[cur].length() + 1];
					wcscpy(input_str, q_responses[cur].c_str());
					SetDlgItemTextW(hWnd, IDC_EDIT1, L"");
					setMode(MODE_NONE);
					onInput.set_value(input_str);
				}
				else if (currentMode == MODE_PAUSE) {
					setMode(MODE_NONE);
					onUnpause.set_value();
				}
				return TRUE;
			}
			}
		}
		return FALSE;
	case APP_LISTCHAR:
		if (currentMode == MODE_INPUT) {
			SetFocus(edit);
			SendMessage(edit, WM_CHAR, wParam, lParam);
		}
		else if (currentMode == MODE_ASK_YN) {
			char c = wctob(towlower(wParam));
			if (c == 'y' || c == 'n') {
				setMode(MODE_ASK_YN);
				onYesNo.set_value(c);
			}
		}
		return TRUE;
	case APP_READQUEUE: {
		std::lock_guard<std::mutex> lock(g_mutex);
		while (!g_queue.empty()) {
			LineEntry& ent = g_queue.front();
			switch (ent.type) {
			case LINE_ADD:
				last_index = ListBox_AddString(list, ent.content.c_str());
				q_responses.push_back(pending);
				pending = L"";
				break;
			case LINE_APPEND: {
				int origlen = ListBox_GetTextLen(list, last_index);
				int len = origlen + ent.content.length() + 1;
				VLA(wchar_t, wstr, len);
				ListBox_GetText(list, last_index, wstr);
				wcscat_s(wstr, len, ent.content.c_str());
				ListBox_DeleteString(list, last_index);
				ListBox_InsertString(list, last_index, wstr);
				VLA_FREE(wstr);
				if (!pending.empty()) {
					q_responses[last_index] = pending;
					pending = L"";
				}
				break;
			}
			case LINE_CLS:
				ListBox_ResetContent(list);
				q_responses.clear();
				pending = L"";
				break;
			case LINE_PENDING:
				pending = ent.content;
				break;
			}
			g_queue.pop();
		}
		return TRUE;
	}
	case APP_GETINPUT:
		onInput = std::move(*(std::promise<wchar_t*>*)lParam);
		ReplyMessage(0L);
		setMode(MODE_INPUT);
		return TRUE;
	case APP_PAUSE:
		onUnpause = std::move(*(std::promise<void>*)lParam);
		ReplyMessage(0L);
		setMode(MODE_PAUSE);
		return TRUE;
	case APP_ASKYN:
		onYesNo = std::move(*(std::promise<char>*)lParam);
		ReplyMessage(0L);
		setMode(MODE_ASK_YN);
		return TRUE;
	case APP_PREUPDATE:
		SetWindowRedraw(list, FALSE);
		return TRUE;
	case APP_UPDATE:
		//SendMessage(list, WM_VSCROLL, SB_BOTTOM, 0L);
		ListBox_SetCurSel(list, last_index);// Just scrolling doesn't work well
		SetWindowRedraw(list, TRUE);
		// this causes unnescessary flickering
		//RedrawWindow(list, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
		return TRUE;
	case APP_PROGRESS:
		setMarquee(wParam == -1);
		if (wParam != -1)
			ProgressBar_SetPos(progress, wParam);
		setMode(MODE_PROGRESS_BAR);
		return TRUE;
	case WM_SIZE: {
		RECT baseunits;
		baseunits.left = baseunits.right = 100;
		baseunits.top = baseunits.bottom = 100;
		MapDialogRect(hWnd, &baseunits);
		int basex = 4 * baseunits.right / 100;
		int basey = 8 * baseunits.bottom / 100;
		long origright = LOWORD(lParam) * 4 / basex;
		long origbottom = HIWORD(lParam) * 8 / basey;
		RECT rbutton1, rbutton2, rlist, redit, rwide;
		const int MARGIN = 7;
		const int PADDING = 2;
		const int BTN_WIDTH = 50;
		const int BTN_HEIGHT = 14;
		// |-----------wide----------|
		// |-----edit-------|        |
		// |       |--btn2--|--btn1--|

		// Button size
		rbutton1 = {
			origright - MARGIN - BTN_WIDTH,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN,
			origbottom - MARGIN };
		MapDialogRect(hWnd, &rbutton1);
		// Progress/staic size
		rwide = {
			MARGIN,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN,
			origbottom - MARGIN };
		MapDialogRect(hWnd, &rwide);
		// Edit size
		redit = {
			MARGIN,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN - BTN_WIDTH - PADDING,
			origbottom - MARGIN };
		MapDialogRect(hWnd, &redit);
		// Button2 size
		rbutton2 = {
			origright - MARGIN - BTN_WIDTH - PADDING - BTN_WIDTH,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN - BTN_WIDTH - PADDING,
			origbottom - MARGIN };
		MapDialogRect(hWnd, &rbutton2);
		// List size
		rlist = {
			MARGIN,
			MARGIN,
			origright - MARGIN,
			origbottom - MARGIN - BTN_HEIGHT - PADDING};
		MapDialogRect(hWnd, &rlist);
#define MoveToRect(hwnd,rect) MoveWindow((hwnd), (rect).left, (rect).top, (rect).right - (rect).left, (rect).bottom - (rect).top, TRUE)
		MoveToRect(buttonNext, rbutton1);
		MoveToRect(edit, redit);
		MoveToRect(list, rlist);
		MoveToRect(progress, rwide);
		MoveToRect(staticText, rwide);
		MoveToRect(buttonYes, rbutton2);
		MoveToRect(buttonNo, rbutton1);
		InvalidateRect(hWnd, &rwide, FALSE); // needed because static doesn't repaint on move
		return TRUE;
	}
	case WM_CLOSE:
		if (con_can_close || MessageBoxW(hWnd, L"Patch configuration is not finished.\n\nQuit anyway?", L"Touhou Community Reliant Automatic Patcher", MB_YESNO | MB_ICONWARNING) == IDYES) {
			EndDialog(hWnd, 0);
		}
		return TRUE;
	case WM_NCDESTROY:
		if (hIconSm) {
			DestroyIcon(hIconSm);
		}
		if (hIcon) {
			DestroyIcon(hIcon);
		}
		return TRUE;
	}
	return FALSE;
}
static bool needAppend = false;
static bool dontUpdate = false;
void log_windows(const char* text) {
	if (!g_console->isAlive())
		return;
	if (!dontUpdate)
		g_console->preupdate();

	WCHAR_T_DEC(text);
	WCHAR_T_CONV(text);
	wchar_t *start = text_w, *end = text_w;
	while (end) {
		int mutexcond = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			end = wcschr(end, '\n');
			if (!end) end = wcschr(start, '\0');
			wchar_t c = *end++;
			if (c == '\n') {
				end[-1] = '\0'; // Replace newline with null
			}
			else if (c == '\0') {
				if (end != text_w && end == start) {
					break; // '\0' right after '\n'
				}
				end = NULL;
			}
			else continue;
			if (needAppend == true) {
				LineEntry le = { LINE_APPEND, start };
				g_queue.push(le);
				needAppend = false;
			}
			else {
				LineEntry le = { LINE_ADD, start };
				g_queue.push(le);
			}
			mutexcond = g_queue.size() > 10;
		}
		if (mutexcond) {
			g_console->readQueue();
			if (dontUpdate) {
				g_console->update();
				g_console->preupdate();
			}
		}
		start = end;
	}
	WCHAR_T_FREE(text);
	if (!end) needAppend = true;

	if (!dontUpdate) {
		g_console->readQueue();
		g_console->update();
	}
}
void log_nwindows(const char* text, size_t len) {
	VLA(char, ntext, len+1);
	memcpy(ntext, text, len);
	ntext[len] = '\0';
	log_windows(ntext);
	VLA_FREE(ntext);
}
/* --- code proudly stolen from thcrap/log.cpp --- */
#define VLA_VSPRINTF(str, va) \
	size_t str##_full_len = _vscprintf(str, va) + 1; \
	VLA(char, str##_full, str##_full_len); \
	/* vs*n*printf is not available in msvcrt.dll. Doesn't matter anyway. */ \
	vsprintf(str##_full, str, va);

void con_vprintf(const char *str, va_list va)
{
	if (str) {
		VLA_VSPRINTF(str, va);
		log_windows(str_full);
		//printf("%s", str_full);
		VLA_FREE(str_full);
	}
}

void con_printf(const char *str, ...)
{
	if (str) {
		va_list va;
		va_start(va, str);
		con_vprintf(str, va);
		va_end(va);
	}
}
/* ------------------- */
void con_clickable(const char* response) {
	WCHAR_T_DEC(response);
	WCHAR_T_CONV(response);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		LineEntry le = { LINE_PENDING,  response_w };
		g_queue.push(le);
	}
	WCHAR_T_FREE(response);
}

void con_clickable(int response) {
	size_t response_full_len = _scprintf("%d", response) + 1;
	VLA(char, response_full, response_full_len);
	sprintf(response_full, "%d", response);
	con_clickable(response_full);
}
void console_init() {
	INITCOMMONCONTROLSEX iccex;
	iccex.dwSize = sizeof(iccex);
	iccex.dwICC = ICC_PROGRESS_CLASS;
	InitCommonControlsEx(&iccex);

	log_set_hook(log_windows, log_nwindows);

	std::promise<void> promise;
	std::future<void> future = promise.get_future();
	std::thread([&promise]() {
		g_console->onInit = std::move(promise);
		g_console->createModal(NULL);
		log_set_hook(NULL, NULL);
		if (!g_exitguithreadevent) {
			exit(0); // Exit the entire process when exiting this thread
		}
		else {
			g_exitguithreadevent.set_value();
		}
	}).detach();
	future.get();
}
wchar_t *console_read() {
	dontUpdate = false;
	g_console->readQueue();
	g_console->update();
	wchar_t *input = g_console->getInput().get();
	needAppend = false; // gotta insert that newline
	return input;
}
void cls(SHORT top) {
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		LineEntry le = { LINE_CLS, L"" };
		g_queue.push(le);
		g_console->readQueue();
		needAppend = false;
	}
	if (dontUpdate) {
		g_console->update();
		g_console->preupdate();
	}
}
void pause(void) {
	dontUpdate = false;
	con_printf("Press ENTER to continue . . . "); // this will ReadQueue and Update for us
	needAppend = false;
	g_console->pause().get();
}
void console_prepare_prompt(void) {
	g_console->preupdate();
	dontUpdate = true;
}
void console_print_percent(int pc) {
	g_console->setProgress(pc);
}
char console_ask_yn(const char* what) {
	dontUpdate = false;
	log_windows(what);
	needAppend = false;

	return g_console->askyn().get();
}
void con_end(void) {
	std::promise<void> promise;
	std::future<void> future = promise.get_future();
	g_exitguithreadevent = std::move(promise);
	SendMessage(g_console->getHandle(), WM_CLOSE, 0, 0L);
	future.get();
}
HWND con_hwnd(void) {
	return g_console->getHandle();
}
int console_width(void) {
	return 80;
}
