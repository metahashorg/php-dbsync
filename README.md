# php-dbsync

This repository contains PHP7 driver and linux daemon service to maintain multiple databases from simple script command.
Currently only Redis DB is supported.

For installation instructions check [install.txt](https://github.com/metahashorg/php-dbsync/blob/master/install.txt).

## Get the source code
Clone the repository by:
```shell
git clone https://github.com/metahashorg/php-dbsync
```

## Communication flow
PHP script calls API function with the given DB command.
DB command is getting sent over network to multiple dbsyncd service instances.
dbsyncd instance receive command and proxy it to multiple DB services.
First successful string result is returned to PHP to make sure at list single DB service applied the command.

## Security
SHA256 signature with RSA public/private keypair can be configured to ensure that only trusted PHP application contact dbsyncd service.
Passwordless private key in PEM format is expected.

## PHP API
```
string dbsync_send(string $command[, string $address])
```
`dbsync_send` sends database command to remote service and returns string result.
Optionally server address can be specified. But server address is expected to be configured in php.ini.

## php.ini
```
dbsync.servers = address1:port1[,address2:port2[,...]]
dbsync.signkey = /path/to/PEM/private/key
dbsync.keepalive = 1
```
`dbsync.servers` is a list of addresses with installed dbsyncd service.

`dbsync.signkey` is an optional parameter. Instructs PHP driver to use signing trust mechanism.

`dbsync.keepalive` is an optional parameter. Instructs PHP driver for connection use mode.
0 is to close connection for each dbsync_send.
1 is to keep connection during script instance run (request processing).
1 is a default mode.

You may find useful to configure these parameters through dbsync.ini file
and put it into PHP configuration as pointed in [install.txt](https://github.com/metahashorg/php-dbsync/blob/master/install.txt).

## dbsyncd service
```
dbsyncd [-b <listen address>] [-p <listen port>] [-s <public key>] [-d <databases>] [-c]
```
`-b <listen address>` IPv4 network address daemon binds to. Default value is 127.0.0.1.

`-p <listen port>` network port where daemon is listening. Default value is 1111.

`-s <public key>` enables signature verification to filter trusted command sources. Parameter points to PEM file with public key.

`-d <databases>` list of databases which dbsyncd will proxy received command. Default is 'redis:127.0.0.1:6379'.

`-c` close connection for each command, default mode to keep connections alive.
If signature verification is enabled connection closed if verification is failed.
Default mode to keep connections alive.

## Debug version
It is possible to build debug version of daemon and PHP extension. Maybe useful to localise problems.
Daemon and PHP will print to stderr additional messages.

To get debug version of dbsyncd daemon rebuild with command:
```shell
make clean
make DEBUG=1
```

To get debug version of PHP extension rebuild with command:
```shell
./configure --enable-dbsync --enable-dbsync-debug
make clean
make
sudo make install
```

