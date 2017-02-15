#include "events.h"
#include "virtual.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "gi.h"
#include "v_text.h"
#include "actor.h"
#include "c_dispatch.h"
#include "d_net.h"

DStaticEventHandler* E_FirstEventHandler = nullptr;
DStaticEventHandler* E_LastEventHandler = nullptr;

bool E_RegisterHandler(DStaticEventHandler* handler)
{
	if (handler == nullptr || handler->ObjectFlags & OF_EuthanizeMe)
		return false;
	if (E_CheckHandler(handler))
		return false;

	handler->OnRegister();
	
	// link into normal list
	// update: link at specific position based on order.
	DStaticEventHandler* before = nullptr;
	for (DStaticEventHandler* existinghandler = E_FirstEventHandler; existinghandler; existinghandler = existinghandler->next)
	{
		if (existinghandler->Order > handler->Order)
		{
			before = existinghandler;
			break;
		}
	}

	// 1. MyHandler2->1:
	//    E_FirstEventHandler = MyHandler2, E_LastEventHandler = MyHandler2
	// 2. MyHandler3->2:
	//    E_FirstEventHandler = MyHandler2, E_LastEventHandler = MyHandler3

	if (before != nullptr)
	{
		// if before is not null, link it before the existing handler.
		// note that before can be first handler, check for this.
		handler->next = before;
		handler->prev = before->prev;
		before->prev = handler;
		if (before == E_FirstEventHandler)
			E_FirstEventHandler = handler;
	}
	else
	{
		// so if before is null, it means add last.
		// it can also mean that we have no handlers at all yet.
		handler->prev = E_LastEventHandler;
		handler->next = nullptr;
		if (E_FirstEventHandler == nullptr)
			E_FirstEventHandler = handler;
		E_LastEventHandler = handler;
		if (handler->prev != nullptr)
			handler->prev->next = handler;
	}

	if (handler->IsStatic())
	{
		handler->ObjectFlags |= OF_Fixed;
		handler->ObjectFlags |= OF_Transient;
	}

	return true;
}

bool E_UnregisterHandler(DStaticEventHandler* handler)
{
	if (handler == nullptr || handler->ObjectFlags & OF_EuthanizeMe)
		return false;
	if (!E_CheckHandler(handler))
		return false;

	handler->OnUnregister();

	// link out of normal list
	if (handler->prev)
		handler->prev->next = handler->next;
	if (handler->next)
		handler->next->prev = handler->prev;
	if (handler == E_FirstEventHandler)
		E_FirstEventHandler = handler->next;
	if (handler == E_LastEventHandler)
		E_LastEventHandler = handler->prev;
	if (handler->IsStatic())
	{
		handler->ObjectFlags &= ~(OF_Fixed|OF_Transient);
		handler->Destroy();
	}
	return true;
}

bool E_CheckHandler(DStaticEventHandler* handler)
{
	for (DStaticEventHandler* lhandler = E_FirstEventHandler; lhandler; lhandler = lhandler->next)
		if (handler == lhandler) return true;
	return false;
}

bool E_IsStaticType(PClass* type)
{
	return (type->IsDescendantOf(RUNTIME_CLASS(DStaticEventHandler)) && // make sure it's from our hierarchy at all.
			!type->IsDescendantOf(RUNTIME_CLASS(DEventHandler)));
}

void E_SerializeEvents(FSerializer& arc)
{
	// todo : stuff
	if (arc.BeginArray("eventhandlers"))
	{
		int numlocalhandlers = 0;
		TArray<DStaticEventHandler*> handlers;
		if (arc.isReading())
		{
			numlocalhandlers = arc.ArraySize();
			// delete all current local handlers, if any
			for (DStaticEventHandler* lhandler = E_FirstEventHandler; lhandler; lhandler = lhandler->next)
				if (!lhandler->IsStatic()) lhandler->Destroy();
		}
		else
		{
			for (DStaticEventHandler* lhandler = E_FirstEventHandler; lhandler; lhandler = lhandler->next)
			{
				if (lhandler->IsStatic()) continue;
				numlocalhandlers++;
				handlers.Push(lhandler);
			}
		}

		for (int i = 0; i < numlocalhandlers; i++)
		{
			// serialize the object properly.
			if (arc.isReading())
			{
				// get object and put it into the array
				DStaticEventHandler* lhandler;
				arc(nullptr, lhandler);
				if (lhandler != nullptr)
					handlers.Push(lhandler);
			}
			else
			{
				::Serialize<DStaticEventHandler>(arc, nullptr, handlers[i], nullptr);
			}
		}

		if (arc.isReading())
		{
			// add all newly deserialized handlers into the list
			for (int i = 0; i < numlocalhandlers; i++)
				E_RegisterHandler(handlers[i]);
		}

		arc.EndArray();
	}
}

static void E_InitStaticHandler(PClass* type, FString typestring, bool map)
{
	if (type == nullptr)
	{
		I_Error("Fatal: unknown event handler class %s in MAPINFO!\n", typestring.GetChars());
		return;

	}

	if (E_IsStaticType(type) && map)
	{
		I_Error("Fatal: invalid event handler class %s in MAPINFO!\nMap-specific event handlers cannot be static.\n", typestring.GetChars());
		return;
	}
	/*
	if (!E_IsStaticType(type) && !map)
	{
		Printf("%cGWarning: invalid event handler class %s in MAPINFO!\nMAPINFO event handlers should inherit Static* directly!\n", TEXTCOLOR_ESCAPE, typestring.GetChars());
		return;
	}*/

	// check if type already exists, don't add twice.
	bool typeExists = false;
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
	{
		if (handler->IsA(type))
		{
			typeExists = true;
			break;
		}
	}

	if (typeExists) return;
	DStaticEventHandler* handler = (DStaticEventHandler*)type->CreateNew();
	E_RegisterHandler(handler);
}

void E_InitStaticHandlers(bool map)
{
	if (savegamerestore)
		return;

	// just make sure
	E_Shutdown(map);

	if (map) // don't initialize map handlers if restoring from savegame.
	{
		// load non-static handlers from gameinfo
		for (unsigned int i = 0; i < gameinfo.EventHandlers.Size(); i++)
		{
			FString typestring = gameinfo.EventHandlers[i];
			PClass* type = PClass::FindClass(typestring);
			if (!type || E_IsStaticType(type)) // don't init the really global stuff here.
				continue;
			E_InitStaticHandler(type, typestring, false);
		}

		for (unsigned int i = 0; i < level.info->EventHandlers.Size(); i++)
		{
			FString typestring = level.info->EventHandlers[i];
			PClass* type = PClass::FindClass(typestring);
			E_InitStaticHandler(type, typestring, true);
		}
	}
	else
	{
		for (unsigned int i = 0; i < gameinfo.EventHandlers.Size(); i++)
		{
			FString typestring = gameinfo.EventHandlers[i];
			PClass* type = PClass::FindClass(typestring);
			if (!type || !E_IsStaticType(type)) // don't init map-local global stuff here.
				continue;
			E_InitStaticHandler(type, typestring, false);
		}
	}
}

void E_Shutdown(bool map)
{
	// delete handlers.
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
	{
		if (handler->IsStatic() == !map)
			handler->Destroy();
	}
}

#define DEFINE_EVENT_LOOPER(name) void E_##name() \
{ \
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next) \
		handler->name(); \
}

// note for the functions below.
// *Unsafe is executed on EVERY map load/close, including savegame loading, etc.
// There is no point in allowing non-static handlers to receive unsafe event separately, as there is no point in having static handlers receive safe event.
// Because the main point of safe WorldLoaded/Unloading is that it will be preserved in savegames.
void E_WorldLoaded()
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
	{
		if (handler->IsStatic()) continue;
		if (savegamerestore) continue; // don't execute WorldLoaded for handlers loaded from the savegame.
		handler->WorldLoaded();
	}
}

void E_WorldUnloaded()
{
	for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
	{
		if (handler->IsStatic()) continue;
		handler->WorldUnloaded();
	}
}

void E_WorldLoadedUnsafe()
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
	{
		if (!handler->IsStatic()) continue;
		handler->WorldLoaded();
	}
}

void E_WorldUnloadedUnsafe()
{
	for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
	{
		if (!handler->IsStatic()) continue;
		handler->WorldUnloaded();
	}
}

void E_WorldThingSpawned(AActor* actor)
{
	// don't call anything if actor was destroyed on PostBeginPlay/BeginPlay/whatever.
	if (actor->ObjectFlags & OF_EuthanizeMe)
		return;
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->WorldThingSpawned(actor);
}

void E_WorldThingDied(AActor* actor, AActor* inflictor)
{
	// don't call anything if actor was destroyed on PostBeginPlay/BeginPlay/whatever.
	if (actor->ObjectFlags & OF_EuthanizeMe)
		return;
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->WorldThingDied(actor, inflictor);
}

void E_WorldThingRevived(AActor* actor)
{
	// don't call anything if actor was destroyed on PostBeginPlay/BeginPlay/whatever.
	if (actor->ObjectFlags & OF_EuthanizeMe)
		return;
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->WorldThingRevived(actor);
}

void E_WorldThingDamaged(AActor* actor, AActor* inflictor, AActor* source, int damage, FName mod, int flags, DAngle angle)
{
	// don't call anything if actor was destroyed on PostBeginPlay/BeginPlay/whatever.
	if (actor->ObjectFlags & OF_EuthanizeMe)
		return;
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->WorldThingDamaged(actor, inflictor, source, damage, mod, flags, angle);
}

void E_WorldThingDestroyed(AActor* actor)
{
	// don't call anything if actor was destroyed on PostBeginPlay/BeginPlay/whatever.
	if (actor->ObjectFlags & OF_EuthanizeMe)
		return;
	// don't call anything for non-spawned things (i.e. those that were created, but immediately destroyed)
	// this is because Destroyed should be reverse of Spawned. we don't want to catch random inventory give failures.
	if (!(actor->ObjectFlags & OF_Spawned))
		return;
	for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
		handler->WorldThingDestroyed(actor);
}

void E_PlayerEntered(int num, bool fromhub)
{
	// this event can happen during savegamerestore. make sure that local handlers don't receive it.
	// actually, global handlers don't want it too.
	if (savegamerestore && !fromhub)
		return;

	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->PlayerEntered(num, fromhub);
}

void E_PlayerRespawned(int num)
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->PlayerRespawned(num);
}

void E_PlayerDied(int num)
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->PlayerDied(num);
}

void E_PlayerDisconnected(int num)
{
	for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
		handler->PlayerDisconnected(num);
}

bool E_Responder(event_t* ev)
{
	if (ev->type == EV_GUI_Event)
	{
		// iterate handlers back to front by order, and give them this event.
		for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
		{
			if (handler->IsUiProcessor && handler->UiProcess(ev))
				return true; // event was processed
		}
	}
	else
	{
		// not sure if we want to handle device changes, but whatevs.
		for (DStaticEventHandler* handler = E_LastEventHandler; handler; handler = handler->prev)
		{
			if (handler->InputProcess(ev))
				return true; // event was processed
		}
	}

	return false;
}

void E_Console(int player, FString name, int arg1, int arg2, int arg3)
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		handler->ConsoleProcess(player, name, arg1, arg2, arg3);
}

bool E_CheckUiProcessors()
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->IsUiProcessor)
			return true;

	return false;
}

bool E_CheckRequireMouse()
{
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->IsUiProcessor && handler->RequireMouse)
			return true;

	return false;
}

// normal event loopers (non-special, argument-less)
DEFINE_EVENT_LOOPER(RenderFrame)
DEFINE_EVENT_LOOPER(RenderOverlay)
DEFINE_EVENT_LOOPER(WorldLightning)
DEFINE_EVENT_LOOPER(WorldTick)

// declarations
IMPLEMENT_CLASS(DStaticEventHandler, false, false);
IMPLEMENT_CLASS(DEventHandler, false, false);
IMPLEMENT_CLASS(DBaseEvent, false, false)
IMPLEMENT_CLASS(DRenderEvent, false, false)
IMPLEMENT_CLASS(DWorldEvent, false, false)
IMPLEMENT_CLASS(DPlayerEvent, false, false)
IMPLEMENT_CLASS(DUiEvent, false, false)
IMPLEMENT_CLASS(DInputEvent, false, false)
IMPLEMENT_CLASS(DConsoleEvent, false, false)

DEFINE_FIELD_X(StaticEventHandler, DStaticEventHandler, Order);
DEFINE_FIELD_X(StaticEventHandler, DStaticEventHandler, IsUiProcessor);
DEFINE_FIELD_X(StaticEventHandler, DStaticEventHandler, RequireMouse);

DEFINE_FIELD_X(RenderEvent, DRenderEvent, ViewPos);
DEFINE_FIELD_X(RenderEvent, DRenderEvent, ViewAngle);
DEFINE_FIELD_X(RenderEvent, DRenderEvent, ViewPitch);
DEFINE_FIELD_X(RenderEvent, DRenderEvent, ViewRoll);
DEFINE_FIELD_X(RenderEvent, DRenderEvent, FracTic);
DEFINE_FIELD_X(RenderEvent, DRenderEvent, Camera);

DEFINE_FIELD_X(WorldEvent, DWorldEvent, IsSaveGame);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, IsReopen);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, Thing);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, Inflictor);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, Damage);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, DamageSource);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, DamageType);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, DamageFlags);
DEFINE_FIELD_X(WorldEvent, DWorldEvent, DamageAngle);

DEFINE_FIELD_X(PlayerEvent, DPlayerEvent, PlayerNumber);
DEFINE_FIELD_X(PlayerEvent, DPlayerEvent, IsReturn);

DEFINE_FIELD_X(UiEvent, DUiEvent, Type);
DEFINE_FIELD_X(UiEvent, DUiEvent, KeyString);
DEFINE_FIELD_X(UiEvent, DUiEvent, KeyChar);
DEFINE_FIELD_X(UiEvent, DUiEvent, MouseX);
DEFINE_FIELD_X(UiEvent, DUiEvent, MouseY);
DEFINE_FIELD_X(UiEvent, DUiEvent, IsShift);
DEFINE_FIELD_X(UiEvent, DUiEvent, IsAlt);
DEFINE_FIELD_X(UiEvent, DUiEvent, IsCtrl);

DEFINE_FIELD_X(InputEvent, DInputEvent, Type);
DEFINE_FIELD_X(InputEvent, DInputEvent, KeyScan);
DEFINE_FIELD_X(InputEvent, DInputEvent, KeyString);
DEFINE_FIELD_X(InputEvent, DInputEvent, KeyChar);
DEFINE_FIELD_X(InputEvent, DInputEvent, MouseX);
DEFINE_FIELD_X(InputEvent, DInputEvent, MouseY);

DEFINE_FIELD_X(ConsoleEvent, DConsoleEvent, Player)
DEFINE_FIELD_X(ConsoleEvent, DConsoleEvent, Name)
DEFINE_FIELD_X(ConsoleEvent, DConsoleEvent, Args)

DEFINE_ACTION_FUNCTION(DStaticEventHandler, SetOrder)
{
	PARAM_SELF_PROLOGUE(DStaticEventHandler);
	PARAM_INT(order);

	if (E_CheckHandler(self))
		return 0;

	self->Order = order;
	return 0;
}

DEFINE_ACTION_FUNCTION(DEventHandler, Create)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	// check if type inherits dynamic handlers
	if (E_IsStaticType(t))
	{
		// disallow static types creation with Create()
		ACTION_RETURN_OBJECT(nullptr);
	}
	// generate a new object of this type.
	ACTION_RETURN_OBJECT(t->CreateNew());
}

DEFINE_ACTION_FUNCTION(DEventHandler, CreateOnce)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	// check if type inherits dynamic handlers
	if (E_IsStaticType(t))
	{
		// disallow static types creation with Create()
		ACTION_RETURN_OBJECT(nullptr);
	}
	// check if there are already registered handlers of this type.
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->GetClass() == t) // check precise class
			ACTION_RETURN_OBJECT(handler);
	// generate a new object of this type.
	ACTION_RETURN_OBJECT(t->CreateNew());
}

DEFINE_ACTION_FUNCTION(DEventHandler, Find)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->GetClass() == t) // check precise class
			ACTION_RETURN_OBJECT(handler);
	ACTION_RETURN_OBJECT(nullptr);
}

DEFINE_ACTION_FUNCTION(DEventHandler, Register)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(handler, DStaticEventHandler);
	if (handler->IsStatic()) ACTION_RETURN_BOOL(false);
	ACTION_RETURN_BOOL(E_RegisterHandler(handler));
}

DEFINE_ACTION_FUNCTION(DEventHandler, Unregister)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(handler, DStaticEventHandler);
	if (handler->IsStatic()) ACTION_RETURN_BOOL(false);
	ACTION_RETURN_BOOL(E_UnregisterHandler(handler));
}

// for static
DEFINE_ACTION_FUNCTION(DStaticEventHandler, Create)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	// static handlers can create any type of object.
	// generate a new object of this type.
	ACTION_RETURN_OBJECT(t->CreateNew());
}

DEFINE_ACTION_FUNCTION(DStaticEventHandler, CreateOnce)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	// static handlers can create any type of object.
	// check if there are already registered handlers of this type.
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->GetClass() == t) // check precise class
			ACTION_RETURN_OBJECT(handler);
	// generate a new object of this type.
	ACTION_RETURN_OBJECT(t->CreateNew());
}

// we might later want to change this
DEFINE_ACTION_FUNCTION(DStaticEventHandler, Find)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(t, DStaticEventHandler);
	for (DStaticEventHandler* handler = E_FirstEventHandler; handler; handler = handler->next)
		if (handler->GetClass() == t) // check precise class
			ACTION_RETURN_OBJECT(handler);
	ACTION_RETURN_OBJECT(nullptr);
}

DEFINE_ACTION_FUNCTION(DStaticEventHandler, Register)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(handler, DStaticEventHandler);
	ACTION_RETURN_BOOL(E_RegisterHandler(handler));
}

DEFINE_ACTION_FUNCTION(DStaticEventHandler, Unregister)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(handler, DStaticEventHandler);
	ACTION_RETURN_BOOL(E_UnregisterHandler(handler));
}

#define DEFINE_EMPTY_HANDLER(cls, funcname) DEFINE_ACTION_FUNCTION(cls, funcname) \
{ \
	PARAM_SELF_PROLOGUE(cls); \
	return 0; \
}

DEFINE_EMPTY_HANDLER(DStaticEventHandler, OnRegister)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, OnUnregister)

DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldLoaded)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldUnloaded)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldThingSpawned)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldThingDied)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldThingRevived)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldThingDamaged)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldThingDestroyed)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldLightning)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, WorldTick)

DEFINE_EMPTY_HANDLER(DStaticEventHandler, RenderFrame)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, RenderOverlay)

DEFINE_EMPTY_HANDLER(DStaticEventHandler, PlayerEntered)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, PlayerRespawned)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, PlayerDied)
DEFINE_EMPTY_HANDLER(DStaticEventHandler, PlayerDisconnected)

DEFINE_EMPTY_HANDLER(DStaticEventHandler, UiProcess);
DEFINE_EMPTY_HANDLER(DStaticEventHandler, InputProcess);

DEFINE_EMPTY_HANDLER(DStaticEventHandler, ConsoleProcess);

// ===========================================
//
//  Event handlers
//
// ===========================================

void DStaticEventHandler::OnRegister()
{
	IFVIRTUAL(DStaticEventHandler, OnRegister)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_OnRegister_VMPtr)
			return;
		VMValue params[1] = { (DStaticEventHandler*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::OnUnregister()
{
	IFVIRTUAL(DStaticEventHandler, OnUnregister)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_OnUnregister_VMPtr)
			return;
		VMValue params[1] = { (DStaticEventHandler*)this };
		GlobalVMStack.Call(func, params, 1, nullptr, 0, nullptr);
	}
}

static DWorldEvent* E_SetupWorldEvent()
{
	static DWorldEvent* e = nullptr;
	if (!e) e = (DWorldEvent*)RUNTIME_CLASS(DWorldEvent)->CreateNew();
	e->IsSaveGame = savegamerestore;
	e->IsReopen = level.FromSnapshot && !savegamerestore; // each one by itself isnt helpful, but with hub load we have savegamerestore==0 and level.FromSnapshot==1.
	e->Thing = nullptr;
	e->Inflictor = nullptr;
	e->Damage = 0;
	e->DamageAngle = 0.0;
	e->DamageFlags = 0;
	e->DamageSource = 0;
	e->DamageType = NAME_None;
	return e;
}

void DStaticEventHandler::WorldLoaded()
{
	IFVIRTUAL(DStaticEventHandler, WorldLoaded)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldLoaded_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldUnloaded()
{
	IFVIRTUAL(DStaticEventHandler, WorldUnloaded)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldUnloaded_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldThingSpawned(AActor* actor)
{
	IFVIRTUAL(DStaticEventHandler, WorldThingSpawned)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldThingSpawned_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		e->Thing = actor;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldThingDied(AActor* actor, AActor* inflictor)
{
	IFVIRTUAL(DStaticEventHandler, WorldThingDied)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldThingDied_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		e->Thing = actor;
		e->Inflictor = inflictor;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldThingRevived(AActor* actor)
{
	IFVIRTUAL(DStaticEventHandler, WorldThingRevived)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldThingRevived_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		e->Thing = actor;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldThingDamaged(AActor* actor, AActor* inflictor, AActor* source, int damage, FName mod, int flags, DAngle angle)
{
	IFVIRTUAL(DStaticEventHandler, WorldThingDamaged)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldThingDamaged_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		e->Thing = actor;
		e->Damage = damage;
		e->DamageSource = source;
		e->DamageType = mod;
		e->DamageFlags = flags;
		e->DamageAngle = angle;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldThingDestroyed(AActor* actor)
{
	IFVIRTUAL(DStaticEventHandler, WorldThingDestroyed)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldThingDestroyed_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		e->Thing = actor;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldLightning()
{
	IFVIRTUAL(DStaticEventHandler, WorldLightning)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldLightning_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::WorldTick()
{
	IFVIRTUAL(DStaticEventHandler, WorldTick)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_WorldTick_VMPtr)
			return;
		DWorldEvent* e = E_SetupWorldEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

static DRenderEvent* E_SetupRenderEvent()
{
	static DRenderEvent* e = nullptr;
	if (!e) e = (DRenderEvent*)RUNTIME_CLASS(DRenderEvent)->CreateNew();
	e->ViewPos = ::ViewPos;
	e->ViewAngle = ::ViewAngle;
	e->ViewPitch = ::ViewPitch;
	e->ViewRoll = ::ViewRoll;
	e->FracTic = ::r_TicFracF;
	e->Camera = ::camera;
	return e;
}

void DStaticEventHandler::RenderFrame()
{
	IFVIRTUAL(DStaticEventHandler, RenderFrame)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_RenderFrame_VMPtr)
			return;
		DRenderEvent* e = E_SetupRenderEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::RenderOverlay()
{
	IFVIRTUAL(DStaticEventHandler, RenderOverlay)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_RenderOverlay_VMPtr)
			return;
		DRenderEvent* e = E_SetupRenderEvent();
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

static DPlayerEvent* E_SetupPlayerEvent()
{
	static DPlayerEvent* e = nullptr;
	if (!e) e = (DPlayerEvent*)RUNTIME_CLASS(DPlayerEvent)->CreateNew();
	e->PlayerNumber = -1;
	e->IsReturn = false;
	return e;
}

void DStaticEventHandler::PlayerEntered(int num, bool fromhub)
{
	IFVIRTUAL(DStaticEventHandler, PlayerEntered)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_PlayerEntered_VMPtr)
			return;
		DPlayerEvent* e = E_SetupPlayerEvent();
		e->IsReturn = fromhub;
		e->PlayerNumber = num;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::PlayerRespawned(int num)
{
	IFVIRTUAL(DStaticEventHandler, PlayerRespawned)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_PlayerRespawned_VMPtr)
			return;
		DPlayerEvent* e = E_SetupPlayerEvent();
		e->PlayerNumber = num;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::PlayerDied(int num)
{
	IFVIRTUAL(DStaticEventHandler, PlayerDied)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_PlayerDied_VMPtr)
			return;
		DPlayerEvent* e = E_SetupPlayerEvent();
		e->PlayerNumber = num;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

void DStaticEventHandler::PlayerDisconnected(int num)
{
	IFVIRTUAL(DStaticEventHandler, PlayerDisconnected)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_PlayerDisconnected_VMPtr)
			return;
		DPlayerEvent* e = E_SetupPlayerEvent();
		e->PlayerNumber = num;
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

static DUiEvent* E_SetupUiEvent()
{
	static DUiEvent* e = nullptr;
	if (!e) e = (DUiEvent*)RUNTIME_CLASS(DUiEvent)->CreateNew();
	e->Type = EV_GUI_None;
	e->IsShift = false;
	e->IsAlt = false;
	e->IsCtrl = false;
	e->MouseX = e->MouseY = 0;
	e->KeyChar = 0;
	e->KeyString = "";
	return e;
}

bool DStaticEventHandler::UiProcess(event_t* ev)
{
	IFVIRTUAL(DStaticEventHandler, UiProcess)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_UiProcess_VMPtr)
			return false;
		DUiEvent* e = E_SetupUiEvent();

		//
		e->Type = (EGUIEvent)ev->subtype;
		// we don't want the modders to remember what weird fields mean what for what events.
		switch (e->Type)
		{
		case EV_GUI_None:
			break;
		case EV_GUI_KeyDown:
		case EV_GUI_KeyRepeat:
		case EV_GUI_KeyUp:
			e->KeyChar = ev->data1;
			e->KeyString.Format("%c", e->KeyChar);
			e->IsShift = !!(ev->data3 & GKM_SHIFT);
			e->IsAlt = !!(ev->data3 & GKM_ALT);
			e->IsCtrl = !!(ev->data3 & GKM_CTRL);
			break;
		case EV_GUI_Char:
			e->KeyChar = ev->data1;
			e->KeyString.Format("%c", e->KeyChar);
			e->IsAlt = !!ev->data2; // only true for Win32, not sure about SDL
			break;
		default: // mouse event
			     // note: SDL input doesn't seem to provide these at all
			//Printf("Mouse data: %d, %d, %d, %d\n", ev->x, ev->y, ev->data1, ev->data2);
			e->MouseX = ev->data1;
			e->MouseY = ev->data2;
			e->IsShift = !!(ev->data3 & GKM_SHIFT);
			e->IsAlt = !!(ev->data3 & GKM_ALT);
			e->IsCtrl = !!(ev->data3 & GKM_CTRL);
			break;
		}

		int processed;
		VMReturn results[1] = { &processed };
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, results, 1, nullptr);
		return !!processed;
	}

	return false;
}

static DInputEvent* E_SetupInputEvent()
{
	static DInputEvent* e = nullptr;
	if (!e) e = (DInputEvent*)RUNTIME_CLASS(DInputEvent)->CreateNew();
	e->Type = EV_None;
	e->KeyScan = 0;
	e->KeyChar = 0;
	e->KeyString = "";
	e->MouseX = e->MouseY = 0;
	return e;
}

bool DStaticEventHandler::InputProcess(event_t* ev)
{
	IFVIRTUAL(DStaticEventHandler, InputProcess)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_InputProcess_VMPtr)
			return false;
		DInputEvent* e = E_SetupInputEvent();

		//
		e->Type = (EGenericEvent)ev->type;
		// we don't want the modders to remember what weird fields mean what for what events.
		switch (e->Type)
		{
		case EV_None:
			break;
		case EV_KeyDown:
		case EV_KeyUp:
			e->KeyScan = ev->data1;
			e->KeyChar = ev->data2;
			e->KeyString.Format("%c", e->KeyChar);
			break;
		case EV_Mouse:
			e->MouseX = ev->x;
			e->MouseY = ev->y;
			break;
		default:
			break; // EV_DeviceChange = wat?
		}

		int processed;
		VMReturn results[1] = { &processed };
		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, results, 1, nullptr);
		return !!processed;
	}

	return false;
}

static DConsoleEvent* E_SetupConsoleEvent()
{
	static DConsoleEvent* e = nullptr;
	if (!e) e = (DConsoleEvent*)RUNTIME_CLASS(DConsoleEvent)->CreateNew();
	e->Player = -1;
	e->Name = "";
	for (size_t i = 0; i < countof(e->Args); i++)
		e->Args[i] = 0;
	return e;
}

void DStaticEventHandler::ConsoleProcess(int player, FString name, int arg1, int arg2, int arg3)
{
	IFVIRTUAL(DStaticEventHandler, ConsoleProcess)
	{
		// don't create excessive DObjects if not going to be processed anyway
		if (func == DStaticEventHandler_ConsoleProcess_VMPtr)
			return;
		DConsoleEvent* e = E_SetupConsoleEvent();

		//
		e->Player = player;
		e->Name = name;
		e->Args[0] = arg1;
		e->Args[1] = arg2;
		e->Args[2] = arg3;

		VMValue params[2] = { (DStaticEventHandler*)this, e };
		GlobalVMStack.Call(func, params, 2, nullptr, 0, nullptr);
	}
}

//
void DStaticEventHandler::OnDestroy()
{
	E_UnregisterHandler(this);
	Super::OnDestroy();
}

// console stuff
// this is kinda like puke, except it distinguishes between local events and playsim events.
CCMD(event)
{
	int argc = argv.argc();

	if (argc < 2 || argc > 5)
	{
		Printf("Usage: event <name> [arg1] [arg2] [arg3]\n");
	}
	else
	{
		int arg[3] = { 0, 0, 0 };
		int argn = MIN<int>(argc - 2, countof(arg));
		for (int i = 0; i < argn; i++)
			arg[i] = atoi(argv[2 + i]);
		// call locally
		E_Console(-1, argv[1], arg[0], arg[1], arg[2]);
	}
}

CCMD(netevent)
{
	if (gamestate != GS_LEVEL/* && gamestate != GS_TITLELEVEL*/) // not sure if this should work in title level, but probably not, because this is for actual playing
	{
		Printf("netevent cannot be used outside of a map.\n");
		return;
	}

	int argc = argv.argc();

	if (argc < 2 || argc > 5)
	{
		Printf("Usage: netevent <name> [arg1] [arg2] [arg3]\n");
	}
	else
	{
		int arg[3] = { 0, 0, 0 };
		int argn = MIN<int>(argc - 2, countof(arg));
		for (int i = 0; i < argn; i++)
			arg[i] = atoi(argv[2 + i]);
		// call networked
		Net_WriteByte(DEM_NETEVENT);
		Net_WriteString(argv[1]);
		Net_WriteByte(argn);
		for (int i = 0; i < argn; i++)
			Net_WriteLong(arg[i]);
	}
}
