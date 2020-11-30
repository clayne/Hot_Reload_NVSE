#include <set>

#include "Hooks_Gameplay.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "SafeWrite.h"
#include "Serialization.h"
#include "GameAPI.h"
#include <share.h>
#include <set>
#include "StringVar.h"
#include "ArrayVar.h"
#include "PluginManager.h"
#include "GameOSDepend.h"
#include "InventoryReference.h"
#include "EventManager.h"

static void HandleMainLoopHook(void);

static const UInt32 kMainLoopHookPatchAddr	= 0x0086B386;	// 7th call BEFORE first call to Sleep in oldWinMain	// 006EEC15 looks best for FO3
static const UInt32 kMainLoopHookRetnAddr	= 0x0086B38B;

__declspec(naked) void MainLoopHook()
{
	__asm
	{
		call	HandleMainLoopHook
		mov	eax, ds:[0x11F4748]
		jmp	kMainLoopHookRetnAddr
	}
}

void ToggleUIMessages(bool bEnable)
{
	// Disable: write an immediate return at function entry
	// Enable: restore the push instruction at function entry
	SafeWrite8((UInt32)QueueUIMessage, bEnable ? 0x55 : 0xC3);
}

bool RunCommand_NS(COMMAND_ARGS, Cmd_Execute cmd)
{
	ToggleUIMessages(false);
	bool cmdResult = cmd(PASS_COMMAND_ARGS);
	ToggleUIMessages(true);

	return cmdResult;
}

// boolean, used by ExtraDataList::IsExtraDefaultForContainer() to determine if ExtraOwnership should be treated
// as 'non-default' for an inventory object. Is 0 in vanilla, set to 1 to make ownership NOT treated as default
// Might be those addresses, used to decide if can be copied
static const UInt32 kExtraOwnershipDefaultSetting  = 0x00411F78;	//	0040A654 in Fallout3 1.7
// Byte array at the end of the sub who is the 4th call in ExtraDataList__RemoveAllCopyableExtra
//don't see a second array.. static const UInt32 kExtraOwnershipDefaultSetting2 = 0x0041FE0D;	//


DWORD g_mainThreadID = 0;

struct QueuedScript
{
	Script				*script;
	TESObjectREFR		*thisObj;
	ScriptEventList		*eventList;
	TESObjectREFR		*containingObj;
	UInt8				arg5;
	UInt8				arg6;
	UInt8				arg7;
	UInt8				pad13;
	UInt32				arg8;

	QueuedScript(Script *_script, TESObjectREFR *_thisObj, ScriptEventList *_eventList, TESObjectREFR *_containingObj, UInt8 _arg5, UInt8 _arg6, UInt8 _arg7, UInt32 _arg8) :
		script(_script), thisObj(_thisObj), eventList(_eventList), containingObj(_containingObj), arg5(_arg5), arg6(_arg6), arg7(_arg7), arg8(_arg8) {}

	void Execute()
	{
		_MESSAGE("%08X\t%08X", script->refID, thisObj ? thisObj->refID : 0);
		ThisStdCall<bool>(0x5E2590, CdeclCall<void*>(0x5E24D0), script, thisObj, eventList, containingObj, arg5, arg6, arg7, arg8);
	}
};

Vector<QueuedScript> s_queuedScripts(0x40);

bool __stdcall AddQueuedScript(Script *script, TESObjectREFR *thisObj, ScriptEventList *eventList, TESObjectREFR *containingObj, UInt8 arg5, UInt8 arg6, UInt8 arg7, UInt32 arg8)
{
	s_queuedScripts.Append(script, thisObj, eventList, containingObj, arg5, arg6, arg7, arg8);
	return true;
}

__declspec(naked) bool __fastcall Hook_ScriptRunner_Run(void *srQueue, int EDX, Script *script, TESObjectREFR *thisObj, ScriptEventList *eventList, TESObjectREFR *containingObj, UInt8 arg5, UInt8 arg6, UInt8 arg7, UInt32 arg8)
{
	__asm
	{
		push	ecx
		call	GetCurrentThreadId
		pop		ecx
		cmp		g_mainThreadID, eax
		jnz		addQueue
		mov		eax, 0x5E2590
		jmp		eax
	addQueue:
		jmp		AddQueuedScript
	}
}

static void HandleMainLoopHook(void)
{
	static bool s_recordedMainThreadID = false;
	if (!s_recordedMainThreadID)
	{
		s_recordedMainThreadID = true;
		g_mainThreadID = GetCurrentThreadId();
		
		PluginManager::Dispatch_Message(0, NVSEMessagingInterface::kMessage_DeferredInit, NULL, 0, NULL);

		WriteRelCall(0x5AC295, (UInt32)Hook_ScriptRunner_Run);
		WriteRelCall(0x5AC368, (UInt32)Hook_ScriptRunner_Run);
		WriteRelCall(0x5AC3A8, (UInt32)Hook_ScriptRunner_Run);
		WriteRelCall(0x5AC3E9, (UInt32)Hook_ScriptRunner_Run);
	}
	else if (!s_queuedScripts.Empty())
	{
		for (auto iter = s_queuedScripts.Begin(); iter; ++iter)
			iter().Execute();
		s_queuedScripts.Clear();
	}

	// if any temporary references to inventory objects exist, clean them up
	if (!s_invRefMap.Empty())
		s_invRefMap.Clear();

	// Tick event manager
	EventManager::Tick();

	// clean up any temp arrays/strings (moved after deffered processing because of array parameter to User Defined Events)
	g_ArrayMap.Clean();
	g_StringMap.Clean();
}

#define DEBUG_PRINT_CHANNEL(idx)								\
																\
static UInt32 __stdcall DebugPrint##idx(const char * str)		\
{																\
	static FILE	* dst = NULL;									\
	if(!dst) dst = _fsopen("nvse_debugprint" #idx ".log", "w", _SH_DENYWR);	\
	if(dst) fputs(str, dst);									\
	return 0;													\
}

DEBUG_PRINT_CHANNEL(0)	// used to exit
DEBUG_PRINT_CHANNEL(1)	// ignored
DEBUG_PRINT_CHANNEL(2)	// ignored
// 3 - program flow
DEBUG_PRINT_CHANNEL(4)	// ignored
// 5 - stack trace?
DEBUG_PRINT_CHANNEL(6)	// ignored
// 7 - ingame
// 8 - ingame

// these are all ignored in-game
static void Hook_DebugPrint(void)
{
	const UInt32	kMessageHandlerVtblBase = 0x010C14A8;

	SafeWrite32(kMessageHandlerVtblBase + (0 * 4), (UInt32)DebugPrint0);
	SafeWrite32(kMessageHandlerVtblBase + (1 * 4), (UInt32)DebugPrint1);
	SafeWrite32(kMessageHandlerVtblBase + (2 * 4), (UInt32)DebugPrint2);
	SafeWrite32(kMessageHandlerVtblBase + (4 * 4), (UInt32)DebugPrint4);
	SafeWrite32(kMessageHandlerVtblBase + (6 * 4), (UInt32)DebugPrint6);
}

void ToggleConsoleOutput(bool enable)
{
	static bool s_bEnabled = true;
	if (enable != s_bEnabled) {
		s_bEnabled = enable;
		if (enable) {
			// original code: 'push ebp ; mov esp, ebp
			SafeWrite8(s_Console__Print+0, 0x55);
			SafeWrite8(s_Console__Print+1, 0x88);
			SafeWrite8(s_Console__Print+2, 0xEC);
		}
		else {
			// 'retn 8'
			SafeWrite8(s_Console__Print, 0xC2);
			SafeWrite8(s_Console__Print+1, 0x08);
			SafeWrite8(s_Console__Print+2, 0x00);
		}
	}
}

static const UInt32 kCreateReferenceCallAddr		= 0x004C37D0;
static const UInt32 kCreateDroppedReferenceHookAddr = 0x0057515A;
static const UInt32 kCreateDroppedReferenceRetnAddr = 0x00575162;

void __stdcall HandleDroppedItem(TESObjectREFR *dropperRef, TESForm *itemBase, TESObjectREFR *droppedRef)
{
	if (droppedRef)
		EventManager::HandleEvent(EventManager::kEventID_OnDrop, dropperRef, droppedRef);
	if (itemBase)
		EventManager::HandleEvent(EventManager::kEventID_OnDropItem, dropperRef, itemBase);
}

__declspec(naked) void DroppedItemHook(void)
{
	__asm
	{
		call	kCreateReferenceCallAddr
		mov	[ebp-4], eax
		push	eax
		push	dword ptr [ebp+8]
		push	dword ptr [ebp-0x20]
		call	HandleDroppedItem
		jmp	kCreateDroppedReferenceRetnAddr
	}
}

static const UInt32 kMainMenuFromIngameMenuPatchAddr = 0x007D0B17;	// 3rd call above first reference to aDataMusicSpecialMaintitle_mp3, call following mov ecx, g_osGlobals
static const UInt32 kMainMenuFromIngameMenuRetnAddr	 = 0x007D0B90;	// original call

static const UInt32 kExitGameViaQQQPatchAddr		 = 0x005B6CA6;	// Inside Cmd_QuitGame_Execute, after mov ecx, g_osGlobals
static const UInt32 kExitGameViaQQQRetnAddr			 = 0x005B6CB0;	// original call

static const UInt32 kExitGameFromMenuPatchAddr       = 0x007D0C3E;	// 2nd call to kExitGameViaQQQRetnAddr

enum QuitGameMessage
{
	kQuit_ToMainMenu,
	kQuit_ToWindows,
	kQuit_QQQ,
};

void __stdcall SendQuitGameMessage(QuitGameMessage msg)
{
	UInt32 msgToSend = NVSEMessagingInterface::kMessage_ExitGame;
	if (msg == kQuit_ToMainMenu)
		msgToSend = NVSEMessagingInterface::kMessage_ExitToMainMenu;
	else if (msg == kQuit_QQQ)
		msgToSend = NVSEMessagingInterface::kMessage_ExitGame_Console;

	PluginManager::Dispatch_Message(0, msgToSend, NULL, 0, NULL);
//	handled by Dispatch_Message EventManager::HandleNVSEMessage(msgToSend, NULL);
}

__declspec(naked) void ExitGameFromMenuHook()
{
	__asm
	{
		mov	byte ptr [ecx+1], 1
		push	kQuit_ToWindows
		call	SendQuitGameMessage
		mov	esp, ebp
		pop	ebp
		retn
	}
}

__declspec(naked) void ExitGameViaQQQHook()
{
	__asm
	{
		mov	byte ptr [ecx+1], 1
		push	kQuit_QQQ
		call	SendQuitGameMessage
		mov	al, 1
		pop	ebp
		retn
	}
}

__declspec(naked) void MainMenuFromIngameMenuHook()
{
	__asm
	{
		mov	byte ptr [ecx+2], 1
		push	kQuit_ToMainMenu
		call	SendQuitGameMessage
		retn
	}
}

void Hook_Gameplay_Init(void)
{
	// game main loop
	// this address was chosen because it is only run when new vegas is in the foreground
	WriteRelJump(kMainLoopHookPatchAddr, (UInt32)&MainLoopHook);

	// hook code that creates a new reference to an item dropped by the player
	WriteRelJump(kCreateDroppedReferenceHookAddr, (UInt32)&DroppedItemHook);

	// hook exit to main menu or to windows
	WriteRelCall(kMainMenuFromIngameMenuPatchAddr, (UInt32)&MainMenuFromIngameMenuHook);
	WriteRelJump(kExitGameViaQQQPatchAddr, (UInt32)&ExitGameViaQQQHook);
	WriteRelJump(kExitGameFromMenuPatchAddr, (UInt32)&ExitGameFromMenuHook);

	// this seems stable and helps in debugging, but it makes large files during gameplay
#if _DEBUG
	UInt32 print;
	if (GetNVSEConfigOption_UInt32("DEBUG", "Print", &print) && print)
		Hook_DebugPrint();
#endif
}

void SetRetainExtraOwnership(bool bRetain)
{
	UInt8 retain = bRetain ? 1 : 0;
	SafeWrite8(kExtraOwnershipDefaultSetting, retain);
	//SafeWrite8(kExtraOwnershipDefaultSetting2, retain);	Not seen (yet?) in Fallout
}
