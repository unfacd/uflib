# uflib

uflib is a basic foundational library used in the application server ufsrv which powers unfacd social network.

ufsrv provides a self-contained environment for managing and servicing distributed,
multi-user, concurrent and real-time communication. 
Whilst ufsrv is currently purpose-built for the needs of unfacd, it can be adapted for other needs, especially if you are targeting a scalable micro-services environment. The base server has direct built-in support for WebSockets, web-based API's, redis and MariaDB.

## Getting Started

uflib is selfcontained and does not depend on the the presence of the ufsrv code.

Also, ufsrv has been previously used to build a TURN server. So really, any web-based service can be pulled together, building on ufrsv's foundational networking and concurrent servicing capabilities.

### Prerequisites

ufsrv is written entirely in c and the basic ufsrv has quite a number of package dependencies. The extract below is from [cmake's build file](https://github.com/unfacd/ufsrv/blob/github/src/CMakeLists.txt)

```
set_target_properties(utf8proc PROPERTIES IMPORTED_LOCATION "/opt/lib/libutf8proc.so")
set_target_properties(lua PROPERTIES IMPORTED_LOCATION "/opt/lib/liblua.a")
set_target_properties(json-c PROPERTIES IMPORTED_LOCATION "/opt/lib/libjson-c.so")
set_target_properties(curl PROPERTIES IMPORTED_LOCATION "/opt/lib/libcurl.a")
set_target_properties(ssl PROPERTIES IMPORTED_LOCATION "/opt/lib/libssl.a")
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION "/opt/lib/libcrypto.a")
set_target_properties(mysqlclient PROPERTIES IMPORTED_LOCATION "/opt/lib/libmysqlclient.so")
```

## Deployment

To deploy this application server you have to have some distributed environment design in mind. It is possible to deploy a single-instance server environment, though, using appropriate parameters in the relevant config file.

In fact, the entire unfacd network has been "miniaturised" into docker, using docker compose, for development purposes.

The server is currently deployed on linux.

### Installing

Limited support is available at this stage.


* [ufsrv-ansible](https://github.com/unfacd/ufsrv-ansible) - Check this repo for ansible assisted build

## Contributing

You are welcome to contribute. Please use github standard workflow.

## Authors

* **Ayman Akt** - *Initial work*

## Acknowledgments

* ufsrv is primarily designed for the unfacd social network, 
where the canonical client software currently in use [unfacd android](https://github.com/unfacd/unfacd-android)  is based on a highly adapted 
version of the Open Whisper Systems' android client. unfacd is unaffiliated with Signal and code reuse is purely based on technical 
considerations and to facilitate rapid prototyping. Naturally, this choice did influence some tradeoffs in the design of ufsrv. However, it is worth
noting that ufsrv does not aim to implement the Signal protocol as a matter of design choice, nor is it necessarily compliant with it.

* Being open source, there are countless instances of code reuse. Those should be acknowledged within the codebase.

## License

Copyright 2015-2019 unfacd works
Licensed under the [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html)

Other copyright notices can also be found within the codebase where relevant.

## Cryptography Notice

This distribution includes cryptographic software. The country in which you currently reside may have restrictions on the import, possession, use, and/or re-export to another country, of encryption software. BEFORE using any encryption software, please check your country's laws, regulations and policies concerning the import, possession, or use, and re-export of encryption software, to see if this is permitted. See (http://www.wassenaar.org/) for more information.



