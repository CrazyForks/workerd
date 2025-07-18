// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// "Actors" are the internal name for Durable Objects, because they implement a sort of actor
// model. We ended up not calling the product "Actors" publicly because we found that people who
// were familiar with actor-model programming were more confused than helped by it -- they tended
// to expect something that looked more specifically like Erlang, whereas our actors are much more
// abstractly related.

#include <workerd/api/http.h>
#include <workerd/io/actor-id.h>
#include <workerd/jsg/jsg.h>

namespace workerd {
template <typename T>
class IoOwn;
}

namespace workerd::api {

// A capability to an ephemeral Actor namespace.
class ColoLocalActorNamespace: public jsg::Object {
 public:
  ColoLocalActorNamespace(uint channel): channel(channel) {}

  jsg::Ref<Fetcher> get(jsg::Lock& js, kj::String actorId);

  JSG_RESOURCE_TYPE(ColoLocalActorNamespace) {
    JSG_METHOD(get);
  }

 private:
  uint channel;
};

class DurableObjectNamespace;

// DurableObjectId type seen by JavaScript.
class DurableObjectId: public jsg::Object {
 public:
  DurableObjectId(kj::Own<ActorIdFactory::ActorId> id): id(kj::mv(id)) {}

  const ActorIdFactory::ActorId& getInner() {
    return *id;
  }

  // ---------------------------------------------------------------------------
  // JS API

  // Converts to a string which can be passed back to the constructor to reproduce the same ID.
  kj::String toString();

  inline bool equals(DurableObjectId& other) {
    return id->equals(*other.id);
  }

  // Get the name, if known.
  inline jsg::Optional<kj::StringPtr> getName() {
    return id->getName();
  }

  JSG_RESOURCE_TYPE(DurableObjectId) {
    JSG_METHOD(toString);
    JSG_METHOD(equals);
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackFieldWithSize("id", sizeof(ActorIdFactory::ActorId));
  }

 private:
  kj::Own<ActorIdFactory::ActorId> id;

  friend class DurableObjectNamespace;
};

// Stub object used to send messages to a remote durable object.
class DurableObject final: public Fetcher {

 public:
  DurableObject(jsg::Ref<DurableObjectId> id,
      IoOwn<OutgoingFactory> outgoingFactory,
      RequiresHostAndProtocol requiresHost)
      : Fetcher(kj::mv(outgoingFactory), requiresHost, true /* isInHouse */),
        id(kj::mv(id)) {}

  jsg::Ref<DurableObjectId> getId() {
    return id.addRef();
  };
  jsg::Optional<kj::StringPtr> getName() {
    return id->getName();
  }

  JSG_RESOURCE_TYPE(DurableObject) {
    JSG_INHERIT(Fetcher);

    JSG_READONLY_INSTANCE_PROPERTY(id, getId);
    JSG_READONLY_INSTANCE_PROPERTY(name, getName);

    JSG_TS_DEFINE(interface DurableObject {
      fetch(request: Request): Response | Promise<Response>;
      alarm?(alarmInfo?: AlarmInvocationInfo): void | Promise<void>;
      webSocketMessage?(ws: WebSocket, message: string | ArrayBuffer): void | Promise<void>;
      webSocketClose?(ws: WebSocket, code: number, reason: string, wasClean: boolean): void | Promise<void>;
      webSocketError?(ws: WebSocket, error: unknown): void | Promise<void>;
    });
    JSG_TS_OVERRIDE(
      type DurableObjectStub<T extends Rpc.DurableObjectBranded | undefined = undefined> =
        Fetcher<T, "alarm" | "webSocketMessage" | "webSocketClose" | "webSocketError">
        & {
          readonly id: DurableObjectId;
          readonly name?: string;
        }
    );
    // Rename this resource type to DurableObjectStub, and make DurableObject
    // the interface implemented by users' Durable Object classes.
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("id", id);
  }

 private:
  jsg::Ref<DurableObjectId> id;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(id);
  }
};

// Like `GlobalActorOutgoingFactory` in the source file, but only used for creating a stub to
// primary DO so the stub can be given to a replica.
//
// The main distinction here is we already have the capability to the primary, so we don't need to
// make an outgoing request to set things up.
class ReplicaActorOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  ReplicaActorOutgoingFactory(kj::Own<IoChannelFactory::ActorChannel> channel, kj::String actorId)
      : actorChannel(kj::mv(channel)),
        actorId(kj::mv(actorId)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override;

 private:
  kj::Own<IoChannelFactory::ActorChannel> actorChannel;
  kj::String actorId;
};

// Global durable object class binding type.
class DurableObjectNamespace: public jsg::Object {
 public:
  // Instead of providing a channel ID, the caller can pass a factory object. This is used in cases
  // where a DurableObjectNamespace is constructed dynamically within an execution context, rather
  // than being a long-lived binding.
  class ActorChannelFactory: public kj::Refcounted {
   public:
    virtual kj::Own<IoChannelFactory::ActorChannel> getGlobalActor(
        const ActorIdFactory::ActorId& id,
        kj::Maybe<kj::String> locationHint,
        ActorGetMode mode,
        bool enableReplicaRouting,
        SpanParent parentSpan) = 0;
  };

  DurableObjectNamespace(uint channel, kj::Own<ActorIdFactory> idFactory)
      : channel(channel),
        idFactory(kj::mv(idFactory)) {}
  DurableObjectNamespace(IoOwn<ActorChannelFactory> factory, kj::Own<ActorIdFactory> idFactory)
      : channel(kj::mv(factory)),
        idFactory(kj::mv(idFactory)) {}

  struct NewUniqueIdOptions {
    // Restricts the new unique ID to a set of colos within a jurisdiction.
    jsg::Optional<kj::String> jurisdiction;

    JSG_STRUCT(jurisdiction);

    JSG_STRUCT_TS_DEFINE(type DurableObjectJurisdiction = "eu" | "fedramp" | "fedramp-high");
    // Possible values from https://developers.cloudflare.com/workers/runtime-apis/durable-objects/#restricting-objects-to-a-jurisdiction
    JSG_STRUCT_TS_OVERRIDE({
      jurisdiction?: DurableObjectJurisdiction;
    });
  };

  // Create a new unique ID for a durable object that will be allocated nearby the calling colo.
  jsg::Ref<DurableObjectId> newUniqueId(jsg::Lock& js, jsg::Optional<NewUniqueIdOptions> options);

  // Create a name-derived ID. Passing in the same `name` (to the same class) will always
  // produce the same ID.
  jsg::Ref<DurableObjectId> idFromName(jsg::Lock& js, kj::String name);

  // Create a DurableObjectId from the stringified form of the ID (as produced by calling
  // `toString()` on a durable object ID). Throws if the ID is not a 64-digit hex number, or if the
  // ID was not originally created for this class.
  //
  // The ID may be one that was originally created using either `newUniqueId()` or `idFromName()`.
  jsg::Ref<DurableObjectId> idFromString(jsg::Lock& js, kj::String id);

  struct GetDurableObjectOptions {
    jsg::Optional<kj::String> locationHint;

    JSG_STRUCT(locationHint);

    JSG_STRUCT_TS_DEFINE(type DurableObjectLocationHint = "wnam" | "enam" | "sam" | "weur" | "eeur" | "apac" | "oc" | "afr" | "me");
    // Possible values from https://developers.cloudflare.com/workers/runtime-apis/durable-objects/#providing-a-location-hint
    JSG_STRUCT_TS_OVERRIDE({
      locationHint?: DurableObjectLocationHint;
    });
  };

  // Gets a durable object by ID or creates it if it doesn't already exist.
  jsg::Ref<DurableObject> get(
      jsg::Lock& js, jsg::Ref<DurableObjectId> id, jsg::Optional<GetDurableObjectOptions> options);

  // Experimental. Gets a durable object by ID if it already exists. Currently, gated for use
  // by cloudflare only.
  jsg::Ref<DurableObject> getExisting(
      jsg::Lock& js, jsg::Ref<DurableObjectId> id, jsg::Optional<GetDurableObjectOptions> options);

  // Creates a subnamespace with the jurisdiction hardcoded.
  jsg::Ref<DurableObjectNamespace> jurisdiction(jsg::Lock& js, kj::String jurisdiction);

  JSG_RESOURCE_TYPE(DurableObjectNamespace, CompatibilityFlags::Reader flags) {
    JSG_METHOD(newUniqueId);
    JSG_METHOD(idFromName);
    JSG_METHOD(idFromString);
    JSG_METHOD(get);
    if (flags.getDurableObjectGetExisting()) {
      JSG_METHOD(getExisting);
    }
    JSG_METHOD(jurisdiction);

    JSG_TS_ROOT();
    if (flags.getDurableObjectGetExisting()) {
      JSG_TS_OVERRIDE(<T extends Rpc.DurableObjectBranded | undefined = undefined> {
        get(id: DurableObjectId, options?: DurableObjectNamespaceGetDurableObjectOptions): DurableObjectStub<T>;
        getExisting(id: DurableObjectId, options?: DurableObjectNamespaceGetDurableObjectOptions): DurableObjectStub<T>;
        jurisdiction(jurisdiction: DurableObjectJurisdiction): DurableObjectNamespace<T>;
      });
    } else {
      JSG_TS_OVERRIDE(<T extends Rpc.DurableObjectBranded | undefined = undefined> {
        get(id: DurableObjectId, options?: DurableObjectNamespaceGetDurableObjectOptions): DurableObjectStub<T>;
        jurisdiction(jurisdiction: DurableObjectJurisdiction): DurableObjectNamespace<T>;
      });
    }
  }

 private:
  kj::OneOf<uint, IoOwn<ActorChannelFactory>> channel;
  kj::Own<ActorIdFactory> idFactory;

  jsg::Ref<DurableObject> getImpl(jsg::Lock& js,
      ActorGetMode mode,
      jsg::Ref<DurableObjectId> id,
      jsg::Optional<GetDurableObjectOptions> options);
};

// DurableObjectClass represents a binding to a Durable Object class that can be used
// as a facet. The only use of this type is to pass to `ctx.facets.get()`.
class DurableObjectClass: public jsg::Object {
 public:
  DurableObjectClass(uint channel): channel(channel) {}
  DurableObjectClass(IoOwn<IoChannelFactory::ActorClassChannel> channel)
      : channel(kj::mv(channel)) {}

  kj::Own<IoChannelFactory::ActorClassChannel> getChannel(IoContext& ioctx);

  JSG_RESOURCE_TYPE(DurableObjectClass) {
    // No methods - this is just a handle that gets passed to ctx.facets.get()
  }

 private:
  kj::OneOf<uint, IoOwn<IoChannelFactory::ActorClassChannel>> channel;
};

#define EW_ACTOR_ISOLATE_TYPES                                                                     \
  api::ColoLocalActorNamespace, api::DurableObject, api::DurableObjectId,                          \
      api::DurableObjectNamespace, api::DurableObjectNamespace::NewUniqueIdOptions,                \
      api::DurableObjectNamespace::GetDurableObjectOptions, api::DurableObjectClass

}  // namespace workerd::api
