//
// Copyright (c) 2008-2016 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once
#if LUTEFISK3D_NETWORK

#include "Lutefisk3D/Network/Connection.h"
#include "Lutefisk3D/Core/Object.h"
#include "Lutefisk3D/IO/VectorBuffer.h"
#include "Lutefisk3D/Container/HashMap.h"

#include <kNet/IMessageHandler.h>
#include <kNet/INetworkServerListener.h>
#include <QtCore/QSet>

namespace Urho3D
{

class HttpRequest;
class MemoryBuffer;
class Scene;

/// MessageConnection hash function.
template <class T> unsigned MakeHash(kNet::MessageConnection* value)
{
    return ((unsigned)(size_t)value) >> 9;
}

/// %Network subsystem. Manages client-server communications using the UDP protocol.
class LUTEFISK3D_EXPORT Network : public Object, public kNet::IMessageHandler, public kNet::INetworkServerListener
{
    URHO3D_OBJECT(Network,Object)

public:
    /// Construct.
    Network(Context* context);
    /// Destruct.
    ~Network() override;

    /// Handle a kNet message from either a client or the server.
    virtual void HandleMessage(kNet::MessageConnection *source, kNet::packet_id_t packetId, kNet::message_id_t msgId, const char *data, size_t numBytes) override;
    /// Compute the content ID for a message.
    virtual u32 ComputeContentID(kNet::message_id_t msgId, const char *data, size_t numBytes) override;
    /// Handle a new client connection.
    virtual void NewConnectionEstablished(kNet::MessageConnection* connection) override;
    /// Handle a client disconnection.
    virtual void ClientDisconnected(kNet::MessageConnection* connection) override;

    /// Connect to a server using UDP protocol. Return true if connection process successfully started.
    bool Connect(const QString& address, unsigned short port, Scene* scene, const VariantMap& identity = Variant::emptyVariantMap);
    /// Disconnect the connection to the server. If wait time is non-zero, will block while waiting for disconnect to finish.
    void Disconnect(int waitMSec = 0);
    /// Start a server on a port using UDP protocol. Return true if successful.
    bool StartServer(unsigned short port);
    /// Stop the server.
    void StopServer();
    /// Broadcast a message with content ID to all client connections.
    void BroadcastMessage(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID = 0);
    /// Broadcast a message with content ID to all client connections.
    void BroadcastMessage(int msgID, bool reliable, bool inOrder, const unsigned char* data, unsigned numBytes, unsigned contentID = 0);
    /// Broadcast a remote event to all client connections.
    void BroadcastRemoteEvent(StringHash eventType, bool inOrder, const VariantMap& eventData = Variant::emptyVariantMap);
    /// Broadcast a remote event to all client connections in a specific scene.
    void BroadcastRemoteEvent(Scene* scene, StringHash eventType, bool inOrder, const VariantMap& eventData = Variant::emptyVariantMap);
    /// Broadcast a remote event with the specified node as a sender. Is sent to all client connections in the node's scene.
    void BroadcastRemoteEvent(Node* node, StringHash eventType, bool inOrder, const VariantMap& eventData = Variant::emptyVariantMap);
    /// Set network update FPS.
    void SetUpdateFps(int fps);
    /// Set simulated latency in milliseconds. This adds a fixed delay before sending each packet.
    void SetSimulatedLatency(int ms);
    /// Set simulated packet loss probability between 0.0 - 1.0.
    void SetSimulatedPacketLoss(float probability);
    /// Register a remote event as allowed to be received. There is also a fixed blacklist of events that can not be allowed in any case, such as ConsoleCommand.
    void RegisterRemoteEvent(StringHash eventType);
    /// Unregister a remote event as allowed to received.
    void UnregisterRemoteEvent(StringHash eventType);
    /// Unregister all remote events.
    void UnregisterAllRemoteEvents();
    /// Set the package download cache directory.
    void SetPackageCacheDir(const QString& path);
    /// Trigger all client connections in the specified scene to download a package file from the server. Can be used to download additional resource packages when clients are already joined in the scene. The package must have been added as a requirement to the scene, or else the eventual download will fail.
    void SendPackageToClients(Scene* scene, PackageFile* package);
    /// Return network update FPS.
    int GetUpdateFps() const { return updateFps_; }
    /// Return simulated latency in milliseconds.
    int GetSimulatedLatency() const { return simulatedLatency_; }
    /// Return simulated packet loss probability.
    float GetSimulatedPacketLoss() const { return simulatedPacketLoss_; }
    /// Return a client or server connection by kNet MessageConnection, or null if none exist.
    Connection* GetConnection(kNet::MessageConnection* connection) const;
    /// Return the connection to the server. Null if not connected.
    Connection* GetServerConnection() const;
    /// Return all client connections.
    std::vector<SharedPtr<Connection> > GetClientConnections() const;
    /// Return whether the server is running.
    bool IsServerRunning() const;
    /// Return whether a remote event is allowed to be received.
    bool CheckRemoteEvent(StringHash eventType) const;
    /// Return the package download cache directory.
    const QString& GetPackageCacheDir() const { return packageCacheDir_; }

    /// Process incoming messages from connections. Called by HandleBeginFrame.
    void Update(float timeStep);
    /// Send outgoing messages after frame logic. Called by HandleRenderUpdate.
    void PostUpdate(float timeStep);

private:
    /// Handle begin frame event.
    void HandleBeginFrame(unsigned, float time_step);
    /// Handle render update frame event.
    void HandleRenderUpdate(float ts);
    /// Handle server connection.
    void OnServerConnected();
    /// Handle server disconnection.
    void OnServerDisconnected();
    /// Reconfigure network simulator parameters on all existing connections.
    void ConfigureNetworkSimulator();

    /// kNet instance.
    kNet::Network* network_;
    /// Client's server connection.
    SharedPtr<Connection> serverConnection_;
    /// Server's client connections.
    HashMap<kNet::MessageConnection*, SharedPtr<Connection> > clientConnections_;
    /// Allowed remote events.
    QSet<StringHash> allowedRemoteEvents_;
    /// Remote event fixed blacklist.
    QSet<StringHash> blacklistedRemoteEvents_;
    /// Networked scenes.
    QSet<Scene*> networkScenes_;
    /// Update FPS.
    int updateFps_;
    /// Simulated latency (send delay) in milliseconds.
    int simulatedLatency_;
    /// Simulated packet loss probability between 0.0 - 1.0.
    float simulatedPacketLoss_;
    /// Update time interval.
    float updateInterval_;
    /// Update time accumulator.
    float updateAcc_;
    /// Package cache directory.
    QString packageCacheDir_;
};

/// Register Network library objects.
void LUTEFISK3D_EXPORT RegisterNetworkLibrary(Context* context);
}
#else
namespace Urho3D
{
// empty type to ease the compilation with networking disabled ( Context uses unique_ptr to this )
struct Network {};
}
#endif
