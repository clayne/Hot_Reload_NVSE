#include <algorithm>
#include <string>
#include <thread>
#include "Utilities.h"
#include "common/IDirectoryIterator.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include <fstream>
#include <sstream>

std::string GetWorkingDir()
{
	char path[MAX_PATH];
	GetCurrentDirectory(MAX_PATH, path);
	return std::string(path) + "\\Data\\Scripts";
}

namespace GeckFuncs
{
	auto CompileScript = reinterpret_cast<void(__thiscall*)(void*, Script*, int)>(0x5C9800);
}

void* g_scriptContext = reinterpret_cast<void*>(0xECFDF8);

void FileWatchThread(int dummy)
{
	try
	{
		auto* handle = FindFirstChangeNotification(GetWorkingDir().c_str(), false, FILE_NOTIFY_CHANGE_LAST_WRITE);
		while (true)
		{
			if (handle == INVALID_HANDLE_VALUE || !handle)
				throw std::exception(FormatString("Could not find directory %s", GetWorkingDir().c_str()).c_str());
			const auto waitStatus = WaitForSingleObject(handle, INFINITE);
			if (waitStatus == WAIT_FAILED)
				throw std::exception("Failed to wait");

			for (IDirectoryIterator iter("Data\\Scripts"); !iter.Done(); iter.Next())
			{
				auto fileName = iter.GetFileName();
				if (ends_with(fileName, ".gek"))
				{
					auto scriptName = fileName.substr(0, fileName.size() - 4);
					auto* form = GetFormByID(scriptName.c_str());
					if (form)
					{
						auto* script = DYNAMIC_CAST(form, TESForm, Script);
						if (script)
						{
							std::ifstream t(iter.GetFullPath());
							std::stringstream buffer;
							buffer << t.rdbuf();
							auto str = buffer.str();
							str = ReplaceAll(str, "\n", "\r\n");
							FormHeap_Free(script->text);
							script->text = static_cast<char*>(FormHeap_Allocate(str.size() + 1));
							strcpy_s(script->text, str.size() + 1, str.c_str());
							GeckFuncs::CompileScript(g_scriptContext, script, 0);
						}
					}
				}
			}
			if (!FindNextChangeNotification(handle))
				throw std::exception("Failed to read again handle.");
		}
	}
	catch (std::exception& e)
	{
		_MESSAGE("Compile from file error: %s (%s)", e.what(), GetLastErrorString().c_str());
	}
}

std::thread g_fileWatchThread;


void InitializeCompileFromFile()
{
	g_fileWatchThread = std::thread(FileWatchThread, 0);
	g_fileWatchThread.detach();
}