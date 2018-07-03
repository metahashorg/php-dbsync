# php-dbsync

This repository contains PHP7 driver and linux daemon service to maintain multiple databases from simple script command.
Currently only Redis DB is supported.

For installation instructions check [Install.txt](https://github.com/metahashorg/php-dbsync/blob/master/install.txt).

## Get the source code
Clone the repository by:
```shell
git clone https://github.com/metahashorg/php-dbsync
```

## Brief description
PHP script calls API function with the given DB command.
DB command is getting sent over network to multiple dbsyncd service instances.
dbsyncd instance receive command and proxy it to multiple DB services.
First successful string result is returned to PHP to make sure at list single DB service applied the command.

## Security
SHA256 signature with RSA public/private keypair can be configured to ensure that only trusted PHP application contacts dbsyncd service.

## PHP API
```
string dbsync_send(string $command[, string $address])
```
`dbsync_send` sends database command to remote service and returns string result.
Optionally server address can be specified. But server address is expected to be configured in php.ini.

## php.ini
```
dbsync.servers = address1:port1[,address2:port2[,...]]
dbsync.signkey = /path/to/private/key
```
`dbsync.servers` is a list of addresses with installed dbsyncd service.
`dbsync.signkey` is an optional parameter. Instructs PHP to use signing trust mechanism.

## dbsyncd service
```
dbsyncd [-b <listen address>] [-p <listen port>] [-s <public key>] [-d <databases>]
```
