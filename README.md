# Payout Module

The Payout Module  acts as a thin layer
around the rather low-level [SSP protocol][itl-ssp]
from Innovative Technology used for communication with the [SMART Hopper][itl-hw-hopper]
and [NV200 Banknote validator][itl-hw-validator] devices.

Doxygen HTML help can be found in docs/doxygen.

## Dependencies

* mandatory for build: `sudo apt install build-essential libhiredis-dev libevent-dev libjansson-dev uuid-dev` 

* runtime: `sudo apt install redis-server`

* development: `sudo apt install doxygen graphviz uuid-runtime valgrind redis-tools`

[read more ...](docs/overview.md)

[itl-ssp]: http://innovative-technology.com/product-files/ssp-manuals/smart-payout-ssp-manual.pdf
[itl-hw-hopper]: http://innovative-technology.com/products/products-main/210-smart-hopper
[itl-hw-validator]: http://innovative-technology.com/products/products-main/90-nv200
