In socket programming, `socket()`, `bind()`, and `connect()` are fundamental functions that are used to establish communication between a client and a server. Here's an explanation of each:

### `socket()`

The `socket()` function creates a new socket, which is an endpoint for communication between two machines. It returns a socket descriptor (a small integer), which can be used in later function calls to refer to this socket.

The function prototype is as follows:

```c
int socket(int domain, int type, int protocol);
```

- **domain**: This specifies the communication domain; for example, `AF_INET` for IPv4 Internet protocols, `AF_INET6` for IPv6, and `AF_UNIX` for local communication.
- **type**: This specifies the communication type; `SOCK_STREAM` for TCP, `SOCK_DGRAM` for UDP, etc.
- **protocol**: This specifies a particular protocol to be used with the socket. Normally only a single protocol exists to support a particular socket type within a given protocol family, in which case protocol can be specified as 0.

### `bind()`

The `bind()` function assigns a local protocol address to a socket. With the `AF_INET` family, this address is the combination of an IPv4 address and a port number.

The function prototype is as follows:

```c
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

- **sockfd**: The socket file descriptor returned by `socket()`.
- **addr**: A pointer to a `sockaddr` structure representing the address to bind to. For `AF_INET`, this is cast to `struct sockaddr_in`.
- **addrlen**: The length of the address structure.

Binding is typically used on the server side to set the port and IP address where the server will listen for incoming connections.

### `connect()`

The `connect()` function is used by a client to establish a connection to a server. It initiates a three-way handshake, which is necessary for a TCP connection.

The function prototype is as follows:

```c
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

- **sockfd**: The socket file descriptor returned by `socket()`.
- **addr**: A pointer to a `sockaddr` structure representing the server address that the client wants to connect to.
- **addrlen**: The length of the address structure.

`connect()` is used by TCP clients to establish a connection to a TCP server identified by the server's IP address and port number.

Here's a simple diagram to visualize the process:

```
Client                          Server
  |                               |
socket() -> sockfd                socket() -> sockfd
  |                               |
  |                             bind() -> (IP:Port)
  |                               |
connect() ----------------------> listen()
  |                               |
  |<------ Three-way handshake ------>|
  |                               |
  |                             accept() -> new sockfd for
  |                               |       the connected client
Communication Established
```

On the server side, `listen()` and `accept()` functions are also used in conjunction with the above functions to complete the setup for a TCP server. The `listen()` function marks the socket as a passive socket that will be used to accept incoming connection requests, and `accept()` actually accepts an incoming connection request and opens a new socket for this connection.
