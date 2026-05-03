# NetLink

A lightweight C++20 static library for local network discovery and peer-to-peer TCP communication.

## Features

- **LAN Discovery** – UDP broadcast-based discovery of hosts on the local network
- **TCP Sessions** – Client/server connection handling with async message passing
- **Cross-project** – Designed to be consumed as a library via CMake/CPM

## Requirements

- C++20 compiler
- CMake 4.0+
- Windows (currently targets `_WIN32_WINNT=0x0A00`)

## Dependencies (fetched automatically via CPM)

- [Asio](https://github.com/chriskohlhoff/asio) – Networking
- [nlohmann/json](https://github.com/nlohmann/json) – JSON serialization

## Usage

Add to your project with CPM:

CPMAddPackage(
    NAME NetLink 
    GITHUB_REPOSITORY Diversiam90815/NetLink 
    VERSION 0.1.0 
)

target_link_libraries(YourApp PRIVATE NetLink)

## Build

cmake -B build -S .cmake --build build

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
