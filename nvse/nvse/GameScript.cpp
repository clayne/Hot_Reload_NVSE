#include "GameAPI.h"
#include "GameScript.h"
#include "GameForms.h"
#include "GameObjects.h"
#include "CommandTable.h"
#include "GameRTTI.h"

UInt32 GetDeclaredVariableType(const char* varName, const char* scriptText)
{
	Tokenizer scriptLines(scriptText, "\n\r");
	std::string curLine;
	while (scriptLines.NextToken(curLine) != -1)
	{
		Tokenizer tokens(curLine.c_str(), " \t\n\r;");
		std::string curToken;

		if (tokens.NextToken(curToken) != -1)
		{
			UInt32 varType = -1;

			// variable declaration?
			if (!StrCompare(curToken.c_str(), "string_var"))
				varType = Script::eVarType_String;
			else if (!StrCompare(curToken.c_str(), "array_var"))
				varType = Script::eVarType_Array;
			else if (!StrCompare(curToken.c_str(), "float"))
				varType = Script::eVarType_Float;
			else if (!StrCompare(curToken.c_str(), "long") || !StrCompare(curToken.c_str(), "int") || !StrCompare(curToken.c_str(), "short"))
				varType = Script::eVarType_Integer;
			else if (!StrCompare(curToken.c_str(), "ref") || !StrCompare(curToken.c_str(), "reference"))
				varType = Script::eVarType_Ref;

			if (varType != -1 && tokens.NextToken(curToken) != -1 && !StrCompare(curToken.c_str(), varName))
			{
				return varType;
			}
		}
	}

	return Script::eVarType_Invalid;
}

Script* GetScriptFromForm(TESForm* form)
{
	TESObjectREFR* refr =  DYNAMIC_CAST(form, TESForm, TESObjectREFR);
	if (refr)
		form = refr->baseForm;

	TESScriptableForm* scriptable = DYNAMIC_CAST(form, TESForm, TESScriptableForm);
	return scriptable ? scriptable->script : NULL;
}

UInt32 Script::GetVariableType(VariableInfo* varInfo)
{
	if (text)
		return GetDeclaredVariableType(varInfo->name.m_data, text);
	else
	{
		// if it's a ref var a matching varIdx will appear in RefList
		if (refList.Contains([&](const RefVariable& ref) { return ref.varIdx == varInfo->idx; }))
			return eVarType_Ref;
		return varInfo->type;
	}
}

bool Script::IsUserDefinedFunction() const
{
	auto* scriptData = static_cast<UInt8*>(data);
	return *(scriptData + 8) == 0x0D;
}

UInt32 Script::GetVarCount() const
{
	// info->varCount include index
	auto count = 0U;
	auto* node = this->varList.Head();
	while (node)
	{
		if (node->data->data)
			++count;
		node = node->Next();
	}
	return count;
}

UInt32 Script::GetRefCount() const
{
	return refList.Count();
}

#if RUNTIME

void Script::RefVariable::Resolve(ScriptEventList * eventList)
{
	if(varIdx && eventList)
	{
		ScriptEventList::Var	* var = eventList->GetVariable(varIdx);
		if(var)
		{
			UInt32	refID = *((UInt32 *)&var->data);
			form = LookupFormByID(refID);
		}
	}
}

CRITICAL_SECTION	csGameScript;				// trying to avoid what looks like concurrency issues

ScriptEventList* Script::CreateEventList(void)
{
	ScriptEventList* result = NULL;
	::EnterCriticalSection(&csGameScript);
	try
	{
#if RUNTIME_VERSION == RUNTIME_VERSION_1_4_0_525
		result = (ScriptEventList*)ThisStdCall(0x005ABF60, this);	// 4th sub above Script::Execute (was 1st above in Oblivion) Execute is the second to last call in Run
#elif RUNTIME_VERSION == RUNTIME_VERSION_1_4_0_525ng
		result = (ScriptEventList*)ThisStdCall(0x005AC110, this);	// 4th sub above Script::Execute (was 1st above in Oblivion) Execute is the second to last call in Run
#else
#error
#endif
	} catch(...) {}

	::LeaveCriticalSection(&csGameScript);
	return result;
}

bool Script::RunScriptLine2(const char * text, TESObjectREFR* object, bool bSuppressOutput)
{
	//ToggleConsoleOutput(!bSuppressOutput);

	ConsoleManager	* consoleManager = ConsoleManager::GetSingleton();

	UInt8	scriptBuf[sizeof(Script)];
	Script	* script = (Script *)scriptBuf;

	CALL_MEMBER_FN(script, Constructor)();
	CALL_MEMBER_FN(script, MarkAsTemporary)();
	CALL_MEMBER_FN(script, SetText)(text);
	bool bResult = CALL_MEMBER_FN(script, Run)(consoleManager->scriptContext, true, object);
	CALL_MEMBER_FN(script, Destructor)();

	//ToggleConsoleOutput(true);
	return bResult;
}

bool Script::RunScriptLine(const char * text, TESObjectREFR * object)
{
	return RunScriptLine2(text, object, false);
}

#endif

Script::RefVariable* ScriptBuffer::ResolveRef(const char* refName)
{
	Script::RefVariable* newRef = NULL;

	// see if it's already in the refList
	auto* foundRef = refVars.FindFirst([&](Script::RefVariable* cur) { return cur->name.m_data && !_stricmp(cur->name.m_data, refName); });
	if (foundRef)
		newRef = foundRef;
	// not in list

	// is it a local ref variable?
	VariableInfo* varInfo = vars.GetVariableByName(refName);
	if (varInfo && GetVariableType(varInfo, NULL) == Script::eVarType_Ref)
	{
		if (!newRef)
			newRef = New<Script::RefVariable>();
		newRef->varIdx = varInfo->idx;
	}
	else // is it a form or global?
	{
		TESForm* form;
		if (_stricmp(refName, "player") == 0)
		{
			// PlayerRef (this is how the vanilla compiler handles it so I'm changing it for consistency and to fix issues)
			form = LookupFormByID(0x14);
		}
		else
		{
			form = GetFormByID(refName);
		}
		if (form)
		{
			TESObjectREFR* refr = DYNAMIC_CAST(form, TESForm, TESObjectREFR);
			if (refr && !refr->IsPersistent()) // only persistent refs can be used in scripts
				return NULL;
			if (!newRef)
				newRef = New<Script::RefVariable>();
			newRef->form = form;
		}
	}

	if (!foundRef && newRef) // got it, add to refList
	{
		newRef->name.Set(refName);
		refVars.Append(newRef);
		info.numRefs++;
	}
	return newRef;
}

UInt32 ScriptBuffer::GetRefIdx(Script::RefVariable* ref)
{
	UInt32 idx = 0;
	for (Script::RefList::_Node* curEntry = refVars.Head(); curEntry && curEntry->data; curEntry = curEntry->next)
	{
		idx++;
		if (ref == curEntry->data)
			break;
	}

	return idx;
}

UInt32 ScriptBuffer::GetVariableType(VariableInfo* varInfo, Script::RefVariable* refVar)
{
	const char* scrText = scriptText;
	if (refVar)
	{
		if (refVar->form)
		{
			TESScriptableForm* scriptable = NULL;
			switch (refVar->form->typeID)
			{
			case kFormType_Reference:
				{
					TESObjectREFR* refr = DYNAMIC_CAST(refVar->form, TESForm, TESObjectREFR);
					scriptable = DYNAMIC_CAST(refr->baseForm, TESForm, TESScriptableForm);
					break;
				}
			case kFormType_Quest:
				scriptable = DYNAMIC_CAST(refVar->form, TESForm, TESScriptableForm);
			}

			if (scriptable && scriptable->script)
			{
				if (scriptable->script->text)
					scrText = scriptable->script->text;
				else
					return scriptable->script->GetVariableType(varInfo);
			}
		}
		else			// this is a ref variable, not a literal form - can't look up script vars
			return Script::eVarType_Invalid;
	}

	return GetDeclaredVariableType(varInfo->name.m_data, scrText);
}

/******************************
 Script
******************************/

class ScriptVarFinder
{
public:
	const char* m_varName;
	ScriptVarFinder(const char* varName) : m_varName(varName)
		{	}
	bool Accept(VariableInfo* varInfo)
	{
		//_MESSAGE("  cur var: %s to match: %s", varInfo->name.m_data, m_varName);
		if (!StrCompare(m_varName, varInfo->name.m_data))
			return true;
		else
			return false;
	}
};

VariableInfo* Script::GetVariableByName(const char* varName)
{
	auto* varIter = varList.Head();
	do
	{
		auto* varInfo = varIter->data;
		if (varInfo && !StrCompare(varName, varInfo->name.m_data))
			return varInfo;
	} while (varIter = varIter->next);
	return NULL;
}

Script::RefVariable	* Script::GetVariable(UInt32 refIdx)
{
	UInt32 idx = 1; // yes, really starts at 1
	if (refIdx)
	{
		auto* varIter = refList.Head();
		do
		{
			if (idx == refIdx)
				return varIter->data;
			idx++;
		} while (varIter = varIter->next);
	}
	return NULL;
}

VariableInfo* Script::GetVariableInfo(UInt32 idx)
{
	for (auto* entry = varList.Head(); entry; entry = entry->next)
		if (entry->data && entry->data->idx == idx)
			return entry->data;

	return NULL;
}

UInt32 Script::AddVariable(TESForm * form)
{
	auto* var = static_cast<RefVariable*>(FormHeap_Allocate(sizeof(RefVariable)));

	var->name.Set("");
	var->form = form;
	var->varIdx = 0;

	refList.Append(var);
	++info.numRefs;
	return info.numRefs;
}

void Script::CleanupVariables(void)
{
	refList.DeleteAll();
}

VariableInfo* Script::VarInfoList::GetVariableByName(const char* varName)
{
	return FindFirst([&](const VariableInfo* v) { return StrCompare(v->name.m_data, varName) == 0; });
}

UInt32 Script::RefList::GetIndex(Script::RefVariable* refVar)
{
	UInt32 idx = 0;
	for (auto cur = this->Begin(); !cur.End(); cur.Next())
	{
		idx++;
		if (*cur == refVar)
			return idx;
	}

	return 0;
}

/***********************************
 ScriptLineBuffer
***********************************/

//const char	* ScriptLineBuffer::kDelims_Whitespace = " \t\n\r";
//const char  * ScriptLineBuffer::kDelims_WhitespaceAndBrackets = " \t\n\r[]";

bool ScriptLineBuffer::Write(const void* buf, UInt32 bufsize)
{
	if (dataOffset + bufsize >= kBufferSize)
		return false;

	memcpy(dataBuf + dataOffset, buf, bufsize);
	dataOffset += bufsize;
	return true;
}

bool ScriptLineBuffer::Write32(UInt32 buf)
{
	return Write(&buf, sizeof(UInt32));
}

bool ScriptLineBuffer::WriteString(const char* buf)
{
	UInt32 len = StrLen(buf);
	if ((len < 0x10000) && Write16(len) && len)
		return Write(buf, len);

	return false;
}

bool ScriptLineBuffer::Write16(UInt16 buf)
{
	return Write(&buf, sizeof(UInt16));
}

bool ScriptLineBuffer::WriteByte(UInt8 buf)
{
	return Write(&buf, sizeof(UInt8));
}

bool ScriptLineBuffer::WriteFloat(double buf)
{
	return Write(&buf, sizeof(double));
}



