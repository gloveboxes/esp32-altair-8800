# Altair FT (File Transfer) App

A BDS C application for CP/M that downloads files from a Remote FT Server.

## Building

```
A> cc ft.c
A> clink ft
```

## Usage

```
A> ft -g <filename>
```

## Examples

```
A> ft -g test.txt
Downloading 'test.txt' from Remote FT Server...
Done (84 bytes)

A> ft -g subdir/hello.c
Downloading 'subdir/hello.c' from Remote FT Server...
Saving as 'hello.c'
Done (256 bytes)
```

## Protocol

The FT app uses I/O ports 60 and 61 to communicate with the firmware:

- **Port 60 (OUT)**: Command port - sends commands to start transfer, request chunks, close
- **Port 60 (IN)**: Status port - returns transfer status (IDLE, DATAREADY, EOF, ERROR)
- **Port 61 (OUT)**: Data port - sends filename characters
- **Port 61 (IN)**: Data port - reads file data bytes

## Requirements

- Altair emulator with WiFi and Remote FT support
- Remote FT Server running on the configured IP address:8090
