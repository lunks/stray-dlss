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
		const bool dump = dumped < 0;   // answered 2026-09-04: 0/8/16/24, 32 bytes
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

	bool set_bytes(const wchar_t *name, const void *src, std::size_t n)
	{
		for (FProperty *p : TFieldRange<FProperty>(fn, EFieldIterationFlags::None))
		{
			if (p == nullptr || !p->HasAnyPropertyFlags(CPF_Parm))
				continue;
			if (p->GetName() != name)
				continue;
			if (static_cast<std::size_t>(p->GetSize()) != n)
			{
				STRAY_LOG_ERROR("dlss-menu: %S is %d bytes, caller has %zu", name, p->GetSize(), n);
				return false;
			}
			std::memcpy(buf.data() + p->GetOffset_Internal(), src, n);
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

UObject *g_row = nullptr;   // BP_HKTextBlock_C, a TextBlock subclass; rooted

// The row: constructed directly (not via Create, which refuses non-UserWidgets), outer = the
// container so it is reachable from a rooted object, rooted itself as well, then added to the
// container's root CanvasPanel and laid out top-left at (200,200) 900x70 - the placement the
// Lua version rendered with. Every step logs; a failure leaves the container on screen.
void build_row()
{
	UClass *rcls = UObjectGlobals::StaticFindObject<UClass *>(nullptr, nullptr,
		STR("/Game/GUI/Widgets/BP_HKTextBlock.BP_HKTextBlock_C"));
	if (rcls == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: row: BP_HKTextBlock_C not resident");
		return;
	}
	FStaticConstructObjectParameters params(rcls, g_widget);
	g_row = UObjectGlobals::StaticConstructObject(params);
	if (g_row == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: row: StaticConstructObject returned null");
		return;
	}
	Root(g_row);
	STRAY_LOG_WARN("dlss-menu: row constructed: %S rootSet=%d", g_row->GetFullName().c_str(),
		g_row->IsRootSet() ? 1 : 0);

	UObject **tree = g_widget->GetValuePtrByPropertyName<UObject *>(STR("WidgetTree"));
	UObject **root = (tree && *tree) ? (*tree)->GetValuePtrByPropertyName<UObject *>(STR("RootWidget")) : nullptr;
	if (root == nullptr || *root == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: row: container has no WidgetTree/RootWidget (tree=%p)",
			tree ? static_cast<void *>(*tree) : nullptr);
		return;
	}
	STRAY_LOG_WARN("dlss-menu: row: container root is %S", (*root)->GetFullName().c_str());

	UFunction *add = find_fn(STR("/Script/UMG.CanvasPanel:AddChildToCanvas"));
	if (add == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: row: CanvasPanel:AddChildToCanvas not found");
		return;
	}
	Args a(add);
	a.set_ptr(STR("Content"), g_row);
	(*root)->ProcessEvent(add, a.buf.data());
	UObject *slot = static_cast<UObject *>(a.ret_ptr());
	if (slot == nullptr)
	{
		STRAY_LOG_ERROR("dlss-menu: row: AddChildToCanvas returned null (root not a CanvasPanel?)");
		return;
	}
	STRAY_LOG_WARN("dlss-menu: row: slot %S", slot->GetFullName().c_str());

	// FAnchors = {FVector2D Minimum, Maximum}, FVector2D = {float X, Y}. Sizes are checked
	// against the property at write time, so a layout mismatch is a logged refusal.
	const float anchors[4] = { 0.f, 0.f, 0.f, 0.f };
	const float pos[2] = { 200.f, 200.f };
	const float size[2] = { 900.f, 70.f };
	struct Call { const wchar_t *fn; const wchar_t *arg; const void *v; std::size_t n; } calls[] = {
		{ STR("/Script/UMG.CanvasPanelSlot:SetAnchors"),  STR("InAnchors"),  anchors, sizeof(anchors) },
		{ STR("/Script/UMG.CanvasPanelSlot:SetPosition"), STR("InPosition"), pos,     sizeof(pos) },
		{ STR("/Script/UMG.CanvasPanelSlot:SetSize"),     STR("InSize"),     size,    sizeof(size) },
	};
	for (const Call &c : calls)
	{
		UFunction *f = find_fn(c.fn);
		if (f == nullptr) { STRAY_LOG_ERROR("dlss-menu: row: %S not found", c.fn); continue; }
		Args s(f);
		if (s.set_bytes(c.arg, c.v, c.n))
			slot->ProcessEvent(f, s.buf.data());
	}
}

void set_text()
{
	if (g_widget == nullptr)
		return;
	if (g_row == nullptr)
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
	g_row->ProcessEvent(fn, a.buf.data());
}

} // namespace

namespace {
void build_row();
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
		// Run 4 (2026-09-04) settled the widget question by measurement: the params layout is
		// 0/8/16/24, the call WORKS - UMG_DebugMenu_C came back created with the menu's own
		// controller as context and owner - and BP_HKTextBlock_C reads isUserWidget=0. It is a
		// TextBlock subclass, and CreateWidget refuses anything that is not a UUserWidget
		// (UserWidget.cpp:2021). So the CONTAINER is the game's UMG_DebugMenu_C (a UserWidget
		// whose root is a CanvasPanel, rendered from Lua, inert) and the ROW is a
		// BP_HKTextBlock_C constructed directly and added to that canvas - the Lua arrangement
		// that rendered once and then died to the collector, now with both objects rooted.
		const wchar_t *kContainer = STR("/Game/GUI/HUD/UMG_DebugMenu.UMG_DebugMenu_C");
		UClass *cls = UObjectGlobals::StaticFindObject<UClass *>(nullptr, nullptr, kContainer);
		UFunction *create = find_fn(STR("/Script/UMG.WidgetBlueprintLibrary:Create"));
		UObject *cdo = UObjectGlobals::StaticFindObject<UObject *>(
			nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
		UObject *ctx = world_context();
		if (cls == nullptr || create == nullptr || cdo == nullptr || ctx == nullptr)
		{
			STRAY_LOG_ERROR("dlss-menu: cannot create: class=%p create=%p cdo=%p world=%p "
				"(class is %S); menu off for this session.",
				static_cast<void *>(cls), static_cast<void *>(create), static_cast<void *>(cdo),
				static_cast<void *>(ctx), kContainer);
			g_failed = true;
			return;
		}
		Args a(create);
		a.set_ptr(STR("WorldContextObject"), ctx);
		a.set_ptr(STR("WidgetType"), cls);
		a.set_ptr(STR("OwningPlayer"), ctx);   // measured working in run 4
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
		build_row();
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
