#include "DlssMenu.hpp"

#include "host/config.hpp"
#include "log.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UnrealFlags.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stray_dlss_menu {
namespace {

using namespace RC;
using namespace RC::Unreal;

struct Preset { const wchar_t *name; int pct; };
const Preset kPresets[] = {
	{ STR("PERFORMANCE"), 50 },
	{ STR("BALANCED"),    58 },
	{ STR("QUALITY"),     67 },
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

int g_sel = 0;
bool g_open = false;
UObject *g_widget = nullptr;   // rooted; see Root() below
bool g_failed = false;

UFunction *find_fn(const wchar_t *path)
{
	return UObjectGlobals::StaticFindObject<UFunction *>(nullptr, nullptr, path);
}

// THE PARAMS BUFFER, BUILT BY PROPERTY OFFSET RATHER THAN BY A HAND-LAID STRUCT.
//
// UE4SS's docs say the return value is the FIRST member of the params struct; UE's own
// convention is parameters in declaration order with the return LAST, and the example that
// claim cites is a function with no parameters, so it does not actually settle the question.
// Rather than bet on either, size the buffer from the function's own property offsets and
// write each argument at the offset the engine reports. That is what UE4SS's Lua bridge does,
// it cannot be wrong about layout, and a wrong layout here is memory corruption rather than a
// failed call.
struct Args
{
	UFunction *fn = nullptr;
	std::vector<std::uint8_t> buf;

	explicit Args(UFunction *f) : fn(f)
	{
		std::size_t size = 0;
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p == nullptr || !p->HasAnyPropertyFlags(CPF_Parm))
				continue;
			const std::size_t end =
				static_cast<std::size_t>(p->GetOffset_Internal()) + static_cast<std::size_t>(p->GetSize());
			if (end > size)
				size = end;
		}
		buf.assign(size, 0);
	}

	// Write a pointer-sized argument by parameter NAME. Names come from the engine, so a typo
	// is a logged miss rather than a silent write to the wrong offset.
	bool set_ptr(const wchar_t *name, void *value)
	{
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p == nullptr || !p->HasAnyPropertyFlags(CPF_Parm))
				continue;
			if (p->GetName() != name)
				continue;
			std::memcpy(buf.data() + p->GetOffset_Internal(), &value, sizeof(value));
			return true;
		}
		STRAY_LOG_ERROR("dlss-menu: parameter %S not found on %S", name,
			fn ? fn->GetName().c_str() : STR("<null>"));
		return false;
	}

	bool set_i32(const wchar_t *name, std::int32_t value)
	{
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p == nullptr || !p->HasAnyPropertyFlags(CPF_Parm))
				continue;
			if (p->GetName() != name)
				continue;
			std::memcpy(buf.data() + p->GetOffset_Internal(), &value, sizeof(value));
			return true;
		}
		return false;
	}

	void *ret_ptr() const
	{
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p != nullptr && p->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				void *v = nullptr;
				std::memcpy(&v, buf.data() + p->GetOffset_Internal(), sizeof(v));
				return v;
			}
		}
		return nullptr;
	}
};

// ROOTING - the entire reason this is in C++.
//
// A widget we construct is referenced by nothing the engine knows about, so the collector takes
// it and the next tick dereferences freed memory. That is the single cause behind nine Lua
// crashes on 2026-09-04: the viewport roots a top-level widget and the game's own UI roots its
// children, but our own hierarchy roots nothing. RF_MarkAsRootSet (UnrealFlags.hpp:69) is
// exposed to C++ via UObject::SetFlags and is NOT reachable from Lua, which is why no
// arrangement of Lua calls could fix it.
void Root(UObject *o)
{
	if (o == nullptr)
		return;
	o->SetFlags(static_cast<EObjectFlags>(RF_MarkAsRootSet | RF_Standalone));
}

void console(const std::wstring &cmd)
{
	UFunction *fn = find_fn(STR("/Script/Engine.KismetSystemLibrary:ExecuteConsoleCommand"));
	UObject *cdo = UObjectGlobals::StaticFindObject<UObject *>(
		nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
	if (fn == nullptr || cdo == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: ExecuteConsoleCommand unavailable");
		return;
	}
	Args a(fn);
	a.set_ptr(STR("WorldContextObject"), nullptr);
	// Command is an FString; write it as a struct-sized copy is unsafe, so use the property.
	for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
	{
		if (p != nullptr && p->GetName() == STR("Command"))
		{
			auto *dst = reinterpret_cast<FString *>(a.buf.data() + p->GetOffset_Internal());
			*dst = FString(cmd.c_str());
		}
	}
	cdo->ProcessEvent(fn, a.buf.data());
	STRAY_LOG_WARN("dlss-menu: console %S", cmd.c_str());
}

std::wstring menu_text()
{
	std::wstring s = STR("=== DLSS QUALITY ===\n");
	for (int i = 0; i < kPresetCount; ++i)
	{
		s += (i == g_sel) ? STR(">  ") : STR("   ");
		s += kPresets[i].name;
		s += STR("\n");
	}
	s += STR("\nLEFT/RIGHT choose - F10 applies");
	return s;
}

void set_text()
{
	if (g_widget == nullptr)
		return;
	UFunction *fn = find_fn(STR("/Script/UMG.TextBlock:SetText"));
	if (fn == nullptr)
		return;
	Args a(fn);
	for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
	{
		if (p != nullptr && p->GetName() == STR("InText"))
		{
			auto *dst = reinterpret_cast<FText *>(a.buf.data() + p->GetOffset_Internal());
			*dst = FText(menu_text().c_str());
		}
	}
	g_widget->ProcessEvent(fn, a.buf.data());
}

} // namespace

void Init()
{
	STRAY_LOG_INFO("dlss-menu: armed. F10 opens, LEFT/RIGHT choose, F10 applies.");
}

void OnToggle()
{
	if (g_failed || !stray_dlss::host::cfg::get_bool("DlssMenu", true))
		return;

	if (g_open)
	{
		g_open = false;
		if (g_widget != nullptr)
		{
			UFunction *rm = find_fn(STR("/Script/UMG.UserWidget:RemoveFromViewport"));
			if (rm != nullptr)
			{
				Args a(rm);
				g_widget->ProcessEvent(rm, a.buf.data());
			}
		}
		wchar_t cmd[64];
		swprintf(cmd, 64, STR("r.ScreenPercentage %d"), kPresets[g_sel].pct);
		console(cmd);
		STRAY_LOG_WARN("dlss-menu: applied %S (%d%%)", kPresets[g_sel].name, kPresets[g_sel].pct);
		return;
	}

	if (g_widget == nullptr)
	{
		// One widget, not a hierarchy: the fewer objects we own, the fewer there are to be
		// collected. A TextBlock is multi-line, so the whole menu fits in one.
		UClass *cls = UObjectGlobals::StaticFindObject<UClass *>(
			nullptr, nullptr, STR("/Script/UMG.TextBlock"));
		UFunction *create = find_fn(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
		UObject *cdo = UObjectGlobals::StaticFindObject<UObject *>(
			nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
		if (cls == nullptr || create == nullptr || cdo == nullptr)
		{
			STRAY_LOG_ERROR("dlss-menu: TextBlock class or Create not resident; menu off for "
				"this session.");
			g_failed = true;
			return;
		}
		Args a(create);
		a.set_ptr(STR("WidgetType"), cls);
		cdo->ProcessEvent(create, a.buf.data());
		g_widget = static_cast<UObject *>(a.ret_ptr());
		if (g_widget == nullptr)
		{
			STRAY_LOG_ERROR("dlss-menu: Create returned null; menu off for this session.");
			g_failed = true;
			return;
		}
		Root(g_widget);   // before anything can tick
		STRAY_LOG_WARN("dlss-menu: widget created and ROOTED at %p",
			static_cast<void *>(g_widget));
	}

	UFunction *add = find_fn(STR("/Script/UMG.UserWidget:AddToViewport"));
	if (add != nullptr)
	{
		Args a(add);
		a.set_i32(STR("ZOrder"), 1000);
		g_widget->ProcessEvent(add, a.buf.data());
	}
	set_text();
	g_open = true;
	STRAY_LOG_WARN("dlss-menu: opened");
}

void OnLeft()
{
	if (!g_open) return;
	g_sel = (g_sel + kPresetCount - 1) % kPresetCount;
	set_text();
}

void OnRight()
{
	if (!g_open) return;
	g_sel = (g_sel + 1) % kPresetCount;
	set_text();
}

} // namespace stray_dlss_menu
