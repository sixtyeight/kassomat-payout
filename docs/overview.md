# SMART Payout Module

> The SMART Payout Module (called Payout afterwards) acts as a thin layer
> around the rather low-level [SSP protocol][itl-ssp]
> from Innovative Technology used for communication with the [SMART Hopper][itl-hw-hopper]
> and [NV200 Banknote validator][itl-hw-validator] devices.

All Interaction with Payout is done asynchronously
using a [Publish/Subscribe][mep-pubsub] message exchange pattern and,
if necessary, a [Request/Response][mep-rr] message exchange pattern. [Redis][redis] is used as the
message broker. The message payload itself is rather simple structured JSON. Payout itself is implemented
in the C programming language.

### Topics used by Payout

Each hardware device has it's own set of topics (specifically the ``event``, ``request``, ``response``, and
``dead-letter`` topics). These queues are prefixed either with ``hopper-`` for the SMART Hopper or ``validator-`` for the NV200 banknote validator.

Using the above naming scheme we end up with the following topics
(topics marked with an asterisk are subscribed by Payout):
 - ``hopper-request`` *
 - ``hopper-response``
 - ``hopper-event``
 - ``hopper-dead-letter``
 - ``validator-request`` *
 - ``validator-response``
 - ``validator-event``
 - ``validator-dead-letter``

### The 'request' / 'response' topics

Those two topics are used in conjunction with each other to implement the aforementioned Request/Response pattern. Messages in a request topic are processed by Payout and the result is published to the response topic.
Every message submitted to a request topic *must* contain a ``msgId`` property. The value of this property is then provided in the resulting response message as the ``correlId`` for correlation. Payout will never publish on it's
own to this topic without a triggering message in the ``request`` topic.

### The 'event' topic

Payout is using this topic for publishing events which have been reported by a device. All messages published here will have at least an ``event`` property. Some events may provide additional properties (e.g. the value of an accepted coin or banknote). A detailed list of all supported events with their properties is enclosed.

### The 'dead-letter' topic

If Payout is not able to interpret a message (e.g. the ``msgId`` property is missing or seriously malformed)
it will publish a copy of that message into this topic. No further processing will be done and no response
message will be published to the ``response`` topic.

[redis]: http://redis.io
[mep-rr]: https://en.wikipedia.org/wiki/Request%E2%80%93response
[mep-pubsub]: https://en.wikipedia.org/wiki/Publish%E2%80%93subscribe_pattern
[itl-ssp]: http://innovative-technology.com/product-files/ssp-manuals/smart-payout-ssp-manual.pdf
[itl-hw-hopper]: http://innovative-technology.com/products/products-main/210-smart-hopper
[itl-hw-validator]: http://innovative-technology.com/products/products-main/90-nv200
