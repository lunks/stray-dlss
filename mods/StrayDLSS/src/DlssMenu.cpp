#include "DlssMenu.hpp"

#include "host/config.hpp"
#include "log.hpp"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UnrealInitializer.hpp>

#include <atomic>

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

// THE THREAD - the second thing Lua could not express, and the cause of the null Create.
//
// register_keydown_event callbacks run on UE4SS's event-loop thread (UE4SSProgram.hpp:280,
// is_event_loop_thread), never the game thread. UWidgetBlueprintLibrary::Create called from
// there returned null with every operand valid (measured 2026-09-04, two runs). UE4SS's own
// Lua ExecuteInGameThread drains its queue from a ProcessEvent PRE-callback (LuaMod.cpp:4094,
// :4183) - the engine calls ProcessEvent on the game thread thousands of times a frame, so
// the first call after a key press is the trampoline. The same shape is used here: the key
// handler stores an action, the callback runs it once it observes Unreal::IsInGameThread().
enum class Action : int { none = 0, toggle, left, right };
std::atomic<int> g_pending{0};
bool g_in_action = false;        // our own ProcessEvent calls re-enter the pre-callback
bool g_logged_thread = false;
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
		// DIAGNOSTIC: a clean null from Create with every operand present is what a WRONG
		// LAYOUT looks like - three writes landing on one offset leaves the context null and
		// Create returns null without a crash. Print the layout once so the log settles it.
		static int dumped = 0;
		const bool dump = dumped < 6;
		if (dump) ++dumped;
		if (dump)
			STRAY_LOG_WARN("dlss-menu: layout of %S:", fn ? fn->GetName().c_str() : STR("<null>"));
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p == nullptr)
				continue;
			if (dump)
				STRAY_LOG_WARN("dlss-menu:   %S off=%d size=%d parm=%d out=%d ret=%d",
					p->GetName().c_str(), p->GetOffset_Internal(), p->GetSize(),
					p->HasAnyPropertyFlags(CPF_Parm) ? 1 : 0,
					p->HasAnyPropertyFlags(CPF_OutParm) ? 1 : 0,
					p->HasAnyPropertyFlags(CPF_ReturnParm) ? 1 : 0);
			if (!p->HasAnyPropertyFlags(CPF_Parm))
				continue;
			const std::size_t end =
				static_cast<std::size_t>(p->GetOffset_Internal()) + static_cast<std::size_t>(p->GetSize());
			if (end > size)
				size = end;
		}
		buf.assign(size, 0);
		if (dump)
			STRAY_LOG_WARN("dlss-menu:   buffer=%zu bytes", size);
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

	bool set_u8(const wchar_t *name, std::uint8_t value)
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
// children, but our own hierarchy roots nothing.
//
// The root set is an INTERNAL flag (EInternalObjectFlags::RootSet on the FUObjectItem), not an
// EObjectFlags one. RF_MarkAsRootSet looks like the answer and is a trap: UE 4.27
// UObjectBase.cpp:181-185 consumes it ONCE, inside AddObject at registration, converts it to the
// internal flag and CLEARS it - so SetFlags(RF_MarkAsRootSet) on an object that already exists
// writes a bit nothing ever reads again. The first version of this file did exactly that; it
// compiled, and it would have crashed like every Lua attempt. UObject::SetRootSet
// (UEPseudo UObject.hpp:290) sets the real one through the object array, and is C++-only.
void Root(UObject *o)
{
	if (o == nullptr)
		return;
	o->SetRootSet();
	if (!o->IsRootSet())
		STRAY_LOG_ERROR("dlss-menu: SetRootSet did not take on %p - expect a crash",
			static_cast<void *>(o));
}

// UWidgetBlueprintLibrary::Create resolves its world through GetWorldFromContextObject, so a
// null context returns null before any widget exists (measured 2026-09-04: "Create returned
// null"). The Lua version passed the player's world; a live PlayerController is the standard
// world-context object and there is one from the main menu onward. Skip the CDO - FindAllOf
// returns it too and it has no world.
UObject *world_context()
{
	std::vector<UObject *> pcs;
	UObjectGlobals::FindAllOf(STR("PlayerController"), pcs);
	for (UObject *o : pcs)
		if (o != nullptr && !o->HasAnyFlags(RF_ClassDefaultObject))
			return o;
	UObject *gi = UObjectGlobals::FindFirstOf(STR("GameInstance"));
	if (gi != nullptr && !gi->HasAnyFlags(RF_ClassDefaultObject))
		return gi;
	return nullptr;
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
	a.set_ptr(STR("WorldContextObject"), world_context());
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
	// BP_HKTextBlock_C is a UserWidget wrapping one UTextBlock in a member named Text. From Lua
	// it was row.Text:SetText(); assigning row.Text directly wrote an FText struct over the
	// widget pointer, which is the same mistake this offset-based path cannot make.
	UObject **inner = g_widget->GetValuePtrByPropertyName<UObject *>(STR("Text"));
	if (inner == nullptr || *inner == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: BP_HKTextBlock_C has no Text member (ptr=%p)",
			static_cast<void *>(inner));
		return;
	}
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
	(*inner)->ProcessEvent(fn, a.buf.data());
}

} // namespace

namespace {
void do_toggle();
void do_left();
void do_right();
} // namespace

void process_event_pre(Hook::TCallbackIterationData<void> &, UObject *, UFunction *fn, void *)
{
	if (g_pending.load(std::memory_order_relaxed) == 0 || g_in_action)
		return;
	if (!IsInGameThread())
		return;
	g_in_action = true;
	const int a = g_pending.exchange(0);
	if (!g_logged_thread)
	{
		g_logged_thread = true;
		STRAY_LOG_WARN("dlss-menu: first action runs on the game thread, inside ProcessEvent(%S)",
			fn ? fn->GetName().c_str() : STR("?"));
	}
	switch (static_cast<Action>(a))
	{
	case Action::toggle: do_toggle(); break;
	case Action::left:   do_left();   break;
	case Action::right:  do_right();  break;
	default: break;
	}
	g_in_action = false;
}

void Init()
{
	Hook::RegisterProcessEventPreCallback(process_event_pre,
		{false, false, STR("StrayDLSS"), STR("DlssMenu")});
	STRAY_LOG_INFO("dlss-menu: armed. F10 opens, LEFT/RIGHT choose, F10 applies. "
		"Actions run on the game thread via the ProcessEvent pre-callback.");
}

void OnToggle() { g_pending.store(static_cast<int>(Action::toggle)); }
void OnLeft()   { g_pending.store(static_cast<int>(Action::left)); }
void OnRight()  { g_pending.store(static_cast<int>(Action::right)); }

namespace {

void do_toggle()
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
		// collected. It must be a UUserWidget child - UE 4.27 UserWidget.cpp:2021 refuses any
		// other class ("CreateWidget can only be used on UUserWidget children") and the first
		// build asked for a plain TextBlock and got null. BP_HKTextBlock_C is the game's own
		// text row: a UserWidget wrapping one TextBlock, in the game's font.
		const wchar_t *kClass = STR("/Game/GUI/Widgets/BP_HKTextBlock.BP_HKTextBlock_C");
		UClass *cls = UObjectGlobals::StaticFindObject<UClass *>(nullptr, nullptr, kClass);
		UFunction *create = find_fn(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
		UObject *cdo = UObjectGlobals::StaticFindObject<UObject *>(
			nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
		UObject *ctx = world_context();
		if (cls == nullptr || create == nullptr || cdo == nullptr || ctx == nullptr)
		{
			STRAY_LOG_ERROR("dlss-menu: cannot create: class=%p create=%p cdo=%p world=%p "
				"(class is %S); menu off for this session.",
				static_cast<void *>(cls), static_cast<void *>(create), static_cast<void *>(cdo),
				static_cast<void *>(ctx), kClass);
			g_failed = true;
			return;
		}
		// Is the class a UUserWidget child at all? ValidateUserWidgetClass (UserWidget.cpp:2021)
		// returns null for anything else, and "HKTextBlock" could as easily be a UTextBlock
		// subclass. IsA on its CDO answers without any unverified accessor.
		{
			UClass *uw = UObjectGlobals::StaticFindObject<UClass *>(nullptr, nullptr,
				STR("/Script/UMG.UserWidget"));
			UObject *tcdo = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr,
				STR("/Game/GUI/Widgets/BP_HKTextBlock.Default__BP_HKTextBlock_C"));
			STRAY_LOG_WARN("dlss-menu: BP_HKTextBlock_C cdo=%p userWidgetClass=%p isUserWidget=%d",
				static_cast<void *>(tcdo), static_cast<void *>(uw),
				(tcdo != nullptr && uw != nullptr && tcdo->IsA(uw)) ? 1 : 0);
		}
		// CONTROL: UMG_DebugMenu_C created and rendered from Lua through this same library
		// call. If it comes back null too, the fault is the call, not the class.
		{
			UClass *dbg = UObjectGlobals::StaticFindObject<UClass *>(nullptr, nullptr,
				STR("/Game/GUI/HUD/UMG_DebugMenu.UMG_DebugMenu_C"));
			if (dbg != nullptr)
			{
				Args c(create);
				c.set_ptr(STR("WorldContextObject"), ctx);
				c.set_ptr(STR("WidgetType"), dbg);
				c.set_ptr(STR("OwningPlayer"), ctx);
				cdo->ProcessEvent(create, c.buf.data());
				UObject *w = static_cast<UObject *>(c.ret_ptr());
				STRAY_LOG_WARN("dlss-menu: CONTROL UMG_DebugMenu_C -> %p %S", static_cast<void *>(w),
					w ? w->GetFullName().c_str() : STR("(null)"));
			}
			else
				STRAY_LOG_WARN("dlss-menu: CONTROL UMG_DebugMenu_C not resident");
		}
		Args a(create);
		a.set_ptr(STR("WorldContextObject"), ctx);
		a.set_ptr(STR("WidgetType"), cls);
		a.set_ptr(STR("OwningPlayer"), ctx);   // the Lua call passed the controller here too
		cdo->ProcessEvent(create, a.buf.data());
		g_widget = static_cast<UObject *>(a.ret_ptr());
		if (g_widget == nullptr)
		{
			STRAY_LOG_ERROR("dlss-menu: Create returned null (world ctx %S); menu off for this "
				"session.", ctx->GetFullName().c_str());
			g_failed = true;
			return;
		}
		Root(g_widget);   // before anything can tick
		STRAY_LOG_WARN("dlss-menu: widget created: %S rootSet=%d",
			g_widget->GetFullName().c_str(), g_widget->IsRootSet() ? 1 : 0);
	}

	UFunction *add = find_fn(STR("/Script/UMG.UserWidget:AddToViewport"));
	if (add != nullptr)
	{
		Args a(add);
		a.set_i32(STR("ZOrder"), 1000);
		g_widget->ProcessEvent(add, a.buf.data());
	}
	UFunction *vis = find_fn(STR("/Script/UMG.Widget:SetVisibility"));
	if (vis != nullptr)
	{
		Args a(vis);
		a.set_u8(STR("InVisibility"), 0);   // ESlateVisibility::Visible
		g_widget->ProcessEvent(vis, a.buf.data());
	}
	set_text();
	g_open = true;
	STRAY_LOG_WARN("dlss-menu: opened");
}

void do_left()
{
	if (!g_open) return;
	g_sel = (g_sel + kPresetCount - 1) % kPresetCount;
	set_text();
}

void do_right()
{
	if (!g_open) return;
	g_sel = (g_sel + 1) % kPresetCount;
	set_text();
}

} // namespace

} // namespace stray_dlss_menu
