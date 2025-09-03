import asyncio
import websockets
import json
import logging
import uuid
import os
from typing import Dict, Optional, Set
from dataclasses import dataclass
from enum import Enum
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

class ConnectionType(Enum):
    WEBSOCKET = "websocket"
    SIGNALING = "signaling"

@dataclass
class BackendService:
    host: str
    port: int
    name: str
    
    @property
    def address(self):
        return f"{self.host}:{self.port}"

class GatewayProxy:
    def __init__(self, signaling_server_url: str, websocket_port: int = 8080):
        self.websocket_port = websocket_port
        self.signaling_server_url = signaling_server_url
        # Backend services that can be port forwarded to
        self.backend_services: Dict[str, BackendService] = {
            "video": BackendService("localhost", 8765, "video"),
        }
        
        # Active connections
        self.active_connections: Dict[str, dict] = {}
        self.authenticated_tokens: Set[str] = {os.getenv('AUTH_TOKEN')}
        # Connection state
        self.websocket_server = None
        
    async def start(self):
        """Start the gateway proxy with dual connection support"""
        logger.info("Starting Gateway Proxy...")
        
        # Start both connection methods concurrently
        await asyncio.gather(
            self._start_websocket_server(),
            self._connect_to_signaling_server(),
            return_exceptions=True
        )
    
    async def _connect_to_signaling_server(self):
        """Connect to the signaling server"""
        while True:
            try:
                logger.info(f"Connecting to signaling server at {self.signaling_server_url}")
                
                async with websockets.connect(self.signaling_server_url) as websocket:
                    self.signaling_connection = websocket
                    connection_id = "signaling_server"
                    initial_message = json.dumps({"role": "robot", "robot_id": os.getenv('ROBOT_ID')})
                    await websocket.send(initial_message)
                    logger.info("Connected to signaling server")
                    await self._handle_connection(websocket, connection_id, ConnectionType.SIGNALING)
                    
            except websockets.exceptions.ConnectionClosed:
                logger.warning("Signaling server connection closed")
            except Exception as e:
                logger.error(f"Error connecting to signaling server: {e}")
            
            # Reconnect after delay
            logger.info("Attempting to reconnect to signaling server in 5 seconds...")
            await asyncio.sleep(5)
    
    async def _start_websocket_server(self):
        """Start WebSocket server for direct LAN connections"""
        logger.info(f"Starting WebSocket server on port {self.websocket_port}")
        
        async def handle_websocket(websocket):
            connection_id = str(uuid.uuid4())
            client_ip = websocket.remote_address[0]
            logger.info(f"New WebSocket connection from {client_ip} (ID: {connection_id})")
            
            try:
                await self._handle_connection(websocket, connection_id, ConnectionType.WEBSOCKET)
            except websockets.exceptions.ConnectionClosed:
                logger.info(f"WebSocket connection {connection_id} closed")
            except Exception as e:
                logger.error(f"Error in WebSocket connection {connection_id}: {e}")
            finally:
                self._cleanup_connection(connection_id)
        
        self.websocket_server = await websockets.serve(
            handle_websocket, 
            "0.0.0.0", 
            self.websocket_port
        )
        logger.info(f"WebSocket server started on port {self.websocket_port}")
        
        # Keep the server running
        await self.websocket_server.wait_closed()

    async def _authenticate_connection(self, websocket, connection_id: str, connection_type: ConnectionType) -> bool:
        """Authenticate incoming connection"""
        try:
            # Wait for authentication message
            if connection_type == ConnectionType.SIGNALING:
                auth_msg = await asyncio.wait_for(websocket.recv(), timeout=None)
            else:
                auth_msg = await asyncio.wait_for(websocket.recv(), timeout=10.0)
            try:
                auth_data = json.loads(auth_msg)
                
                logger.info(f"Authentication data: {auth_data}")
            except json.JSONDecodeError:
                websocket.send(json.dumps({
                    "type": "error",
                    "message": "Authentication successful"
                }))
                logger.error("Invalid authentication format")

                return False
            
            token = auth_data.get("token")
            if not token or token not in self.authenticated_tokens:
                logger.error("Invalid authentication token")
                websocket.send(json.dumps({
                    "type": "error",
                    "message": "Invalid authentication token"
                }))

                return False
            
            # Send authentication success
            await websocket.send(json.dumps({
                "type": "auth_success",
                "message": "Authentication successful"
            }))
            
            return True
            
        except asyncio.TimeoutError:
            logger.error("Authentication timeout")
            return False
        except Exception as e:
            logger.error(f"Authentication error for {connection_id}: {e}")
            return False

    async def _handle_connection(self, websocket, connection_id: str, connection_type: ConnectionType):
        """Handle incoming connection (WebSocket or Signaling)"""
        # Authenticate the connection
        if not await self._authenticate_connection(websocket, connection_id, connection_type):
            logger.warning(f"Authentication failed for connection {connection_id}")
            # if connection_type == ConnectionType.WEBSOCKET:
            #     await websocket.close(code=1008, reason="Authentication failed")
            return
        
        logger.info(f"Connection {connection_id} authenticated successfully")
        
        # Store connection info
        self.active_connections[connection_id] = {
            "websocket": websocket,
            "type": connection_type,
            "authenticated": True,
            "backend_connection": None
        }
        
        # Handle messages
        try:
            async for message in websocket:
                data = json.loads(message)
                logger.info(f"Received message: {data}")
                if(data.get("type") == "connection_closed"):
                    logger.info(f"Connection {connection_id} closed, reported by signaling server")
                    self.active_connections[connection_id] = {
                        "websocket": websocket,
                        "type": connection_type,
                        "authenticated": False,
                        "backend_connection": None
                    }
                    await self._handle_connection(websocket, connection_id, ConnectionType.SIGNALING)
                    return
                await self._handle_message(connection_id, message)
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"Connection {connection_id} closed")
        except Exception as e:
            logger.error(f"Error handling connection {connection_id}: {e}")
    
    async def _handle_message(self, connection_id: str, message: str):
        """Handle incoming message from authenticated connection"""
        logger.info(f"Received message: {message}")
        connection_info = self.active_connections.get(connection_id)
        if not connection_info:
            logger.error(f"No connection info found for {connection_id}")
            return
        
        websocket = connection_info["websocket"]
        
        try:
            data = json.loads(message)
        except json.JSONDecodeError:
            await self._send_error(websocket, "Invalid message format")
            return
        
        message_type = data.get("type")
    
        await self._handle_service_routing(connection_id, data)

    async def _handle_service_routing(self, connection_id: str, data: dict):
        """Route message to specific backend service"""
        service_name = data.get("service")
        
        if service_name not in self.backend_services:
            connection_info = self.active_connections[connection_id]
            logger.error(f"Unknown service: {service_name}")
            return
        
        backend_service = self.backend_services[service_name]
        
        try:
            # Connect to backend service if not already connected
            await self._ensure_backend_connection(connection_id, backend_service)
            
            # Forward message to backend
            connection_info = self.active_connections[connection_id]
            backend_ws = connection_info["backend_connection"]
            
            if backend_ws:
                await backend_ws.send(json.dumps(data))
                logger.info(f"Forwarded message from {connection_id} to {service_name}")
            
        except Exception as e:
            logger.error(f"Error routing to service {service_name}: {e}")
    
    async def _ensure_backend_connection(self, connection_id: str, backend_service: BackendService):
        """Ensure connection to backend service exists"""
        connection_info = self.active_connections[connection_id]
        
        if connection_info["backend_connection"] is None:
            try:
                backend_url = f"ws://{backend_service.address}"
                backend_ws = await websockets.connect(backend_url)
                connection_info["backend_connection"] = backend_ws
                
                logger.info(f"Connected to backend service {backend_service.name} at {backend_service.address}")
                
                # Start listening for backend messages
                asyncio.create_task(self._handle_backend_messages(connection_id, backend_ws))
                
            except Exception as e:
                logger.error(f"Failed to connect to backend {backend_service.name}: {e}")
                raise
    
    async def _handle_backend_messages(self, connection_id: str, backend_ws):
        """Handle messages from backend service"""
        try:
            async for message in backend_ws:
                logger.info(f"Received message from backend: {message}")
                connection_info = self.active_connections.get(connection_id)
                if connection_info:
                    client_ws = connection_info["websocket"]
                    await client_ws.send(message)
                else:
                    # Client disconnected, close backend connection
                    await backend_ws.close()
                    break
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"Backend connection closed for client {connection_id}")
        except Exception as e:
            logger.error(f"Error handling backend messages for {connection_id}: {e}")
    
    def _cleanup_connection(self, connection_id: str):
        """Clean up connection resources"""
        if connection_id in self.active_connections:
            connection_info = self.active_connections[connection_id]
            backend_connection = connection_info.get("backend_connection")
            
            if backend_connection:
                asyncio.create_task(backend_connection.close())
            
            del self.active_connections[connection_id]
            logger.info(f"Cleaned up connection {connection_id}")
    
# Example usage
async def main():
    # Create gateway proxy
    gateway = GatewayProxy(
        signaling_server_url="wss://6e2ea1cfc65c.ngrok-free.app",
        websocket_port=8080
    )
    
    try:
        await gateway.start()
    except KeyboardInterrupt:
        logger.info("Gateway proxy shutting down...")

if __name__ == "__main__":
    asyncio.run(main())