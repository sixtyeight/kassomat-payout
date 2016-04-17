# SMART Payout Module

> The SMART Payout Module (called Payout afterwards) acts as a thin layer
> around the rather low-level [SSP protocol][itl-ssp]
> from Innovative Technology used for communication with the [SMART Hopper][itl-hw-hopper]
> and [NV200 Banknote validator][itl-hw-validator] devices.

All Interaction with Payout is done asynchronously
using a [Publish/Subscribe][mep-pubsub] message exchange pattern and,
if necessary, a [Request/Response][mep-rr] message exchange pattern. [Redis][redis] is used as the
message broker. The message payload is a rather simple structured JSON. Payout itself is implemented
in the C programming language. Some examples can be seen [here](examples.md).
Take also a look at the [Changeomatic][changeomatic], a proof of concept money changer in Java.

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

#### The 'request' / 'response' topics

Those two topics are used in conjunction with each other to implement the aforementioned Request/Response pattern. Messages in a request topic are processed by Payout and the result is published to the response topic.
Every message submitted to a request topic *must* contain a ``msgId`` property. The value of this property is then provided in the resulting response message as the ``correlId`` for correlation. Payout will never publish on it's
own to this topic without a triggering message in the ``request`` topic. A list of supported commands and their properties is enclosed.

#### The 'event' topic

Payout is using this topic for publishing events which have been reported by a device. All messages published here will have at least an ``event`` property. Some events may provide additional properties (e.g. the value of an accepted coin or banknote). A detailed list of all supported events with their properties is enclosed.
As an example, this ``{"event":"credit","amount":1000,"channel":2}`` will be published if a 10 Euro banknote
has been accepted and the amount (which is provided in cents) can be credited. Or, in this example the hopper has accepted a 2 Euro coin: ``{"event":"coin credit","amount":200,"cc":"EUR"}``.

#### The 'dead-letter' topic

> This is not implemented right now

If Payout is not able to interpret a message (e.g. the ``msgId`` property is missing or otherwise seriously malformed)
it will publish a copy of that message into this topic. No further processing will be done and no response
message will be published to the ``response`` topic.

## Overview of Events, Requests and Responses

> This section is still work in progress.

### Events published to the 'hopper-event' topic

``{"event":"read","channel":%ld}``

``{"event":"reading"}``

``{"event":"floating","amount":%ld,"cc":"%s"}``

``{"event":"floated","amount":%ld,"cc":"%s"}``

``{"event":"dispensing","amount":%ld}``

``{"event":"dispensed","amount":%ld}``

``{"event":"cashbox paid","amount":%ld,"cc":"%s"}``

``{"event":"jammed"}``

``{"event":"coin credit","amount":%ld,"cc":"%s"}``

``{"event":"empty"}``

``{"event":"emptying"}``

``{"event":"smart emptying","amount":%ld,"cc":"%s"}``

``{"event":"smart emptied","amount":%ld,"cc":"%s"}``

``{"event":"credit","channel":%ld,"cc":"%s"}``

``{"event":"incomplete payout","dispensed":%ld,"requested":%ld,"cc":"%s"}``

``{"event":"incomplete float","dispensed":%ld,"requested":%ld,"cc":"%s"}``

``{"event":"disabled"}``

``{"event":"calibration fail","error":"no error"}``

``{"event":"calibration fail","error":"sensor flap"}``

``{"event":"calibration fail","error":"sensor exit"}``

``{"event":"calibration fail","error":"sensor coil 1"}``

``{"event":"calibration fail","error":"sensor coil 2"}``

``{"event":"calibration fail","error":"not initialized"}``

``{"event":"calibration fail","error":"checksum error"}``

``{"event":"recalibrating"}``

### Events published to the 'validator-event' topic

``{"event":"read","amount":%ld,"channel":%ld}``

``{"event":"reading"}``

``{"event":"empty"}``

``{"event":"emptying"}``

``{"event":"credit","amount":%ld,"channel":%ld}``

``{"event":"incomplete payout","dispensed":%ld,"requested":%ld,"cc":"%s"}``

``{"event":"incomplete float","dispensed":%ld,"requested":%ld,"cc":"%s"}``

``{"event":"rejecting"}``

``{"event":"rejected"}``

``{"event":"stacking"}``

``{"event":"stored"}``

``{"event":"stacked"}``

``{"event":"safe jam"}``

``{"event":"unsafe jam"}``

``{"event":"disabled"}``

``{"event":"fraud attempt","dispensed":%ld}``

``{"event":"stacker full"}``

``{"event":"cashbox removed"}``

``{"event":"cashbox replaced"}``

``{"event":"cleared from front"}``

``{"event":"cleared into cashbox"}``

``{"event":"calibration fail","error":"no error"}``

``{"event":"calibration fail","error":"sensor flap"}``

``{"event":"calibration fail","error":"sensor exit"}``

``{"event":"calibration fail","error":"sensor coil 1"}``

``{"event":"calibration fail","error":"sensor coil 2"}``

``{"event":"calibration fail","error":"not initialized"}``

``{"event":"calibration fail","error":"checksum error"}``

``{"event":"recalibrating"}``

### Messages for the 'hopper-request' topic

``{"cmd":"get-firmware-version","msgId":"%s"}``

``{"cmd":"get-dataset-version","msgId":"%s"}``

``{"cmd":"set-denomination-level","amount":%ld,"level":%ld,msgId":"%s"}``

``{"cmd":"get-all-levels","msgId":"%s"}``

``{"cmd":"empty","msgId":"%s"}``

``{"cmd":"smart-empty","msgId":"%s"}``

``{"cmd":"enable","msgId":"%s"}``

``{"cmd":"disable","msgId":"%s"}``

``{"cmd":"test-payout","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

``{"cmd":"do-payout","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

``{"cmd":"test-float","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

``{"cmd":"do-float","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

### Messages for the 'validator-request' topic

``{"cmd":"get-firmware-version","msgId":"%s"}``

``{"cmd":"get-dataset-version","msgId":"%s"}``

``{"cmd":"channel-security","msgId":"%s"}``

``{"cmd":"empty","msgId":"%s"}``

``{"cmd":"enable","msgId":"%s"}``

``{"cmd":"disable","msgId":"%s"}``

``{"cmd":"test-payout","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

``{"cmd":"do-payout","amount":%ld,"msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","error":"not enough value in smart payout"}``
  - ``{"correlId":"%s","error":"can't pay exact amount"}``
  - ``{"correlId":"%s","error":"smart payout busy"}``
  - ``{"correlId":"%s","error":"smart payout disabled"}``
  - ``{"correlId":"%s","error":"unknown"}``

``{"cmd":"enable-channels","channels":"%s","msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","result":"failed"}``

``{"cmd":"disable-channels","channels":"%s","msgId":"%s"}``

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","result":"failed"}``

``{"cmd":"inhibit-channels","channels":"%s","msgId":"%s"}`` (do not use for now, needs a fix)

  - ``{"correlId":"%s","result":"ok"}``
  - ``{"correlId":"%s","result":"failed"}``

``{"cmd":"last-reject-note","msgId":"%s"}``

  - ``{"correlId":"%s","reason":"%s","code":%ld}"``
  - ``{"correlId":"%s","reason":"note accepted"}``
  - ``{"correlId":"%s","reason":"note length incorrect"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 2)"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 3)"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 4)"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 5)"}``
  - ``{"correlId":"%s","reason":"channel inhibited"}``
  - ``{"correlId":"%s","reason":"second note inserted"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 8)"}``
  - ``{"correlId":"%s","reason":"note recognised in more than one channel"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 10)"}``
  - ``{"correlId":"%s","reason":"note too long"}``
  - ``{"correlId":"%s","reason":"undisclosed (reject reason 12)"}``
  - ``{"correlId":"%s","reason":"mechanism slow/stalled"}``
  - ``{"correlId":"%s","reason":"strimming attempt detected"}``
  - ``{"correlId":"%s","reason":"fraud channel reject"}``
  - ``{"correlId":"%s","reason":"no notes inserted"}``
  - ``{"correlId":"%s","reason":"peak detect fail"}``
  - ``{"correlId":"%s","reason":"twisted note detected"}``
  - ``{"correlId":"%s","reason":"escrow time-out"}``
  - ``{"correlId":"%s","reason":"bar code scan fail"}``
  - ``{"correlId":"%s","reason":"rear sensor 2 fail"}``
  - ``{"correlId":"%s","reason":"slot fail 1"}``
  - ``{"correlId":"%s","reason":"slot fail 2"}``
  - ``{"correlId":"%s","reason":"lens over-sample"}``
  - ``{"correlId":"%s","reason":"width detect fail"}``
  - ``{"correlId":"%s","reason":"short note detected"}``
  - ``{"correlId":"%s","reason":"note payout"}``
  - ``{"correlId":"%s","reason":"unable to stack note"}``
  - ``{"correlId":"%s","reason":"undefined"}``

### Known issues

 - SSP command ``channel-security`` should return a value of 4 if a channel is inhibited, in reality it doesn't.
 - SPP command ``get-firmware-version`` should return a "variable length string", in reality it doesn't.
 - SSP command ``get-dataset-version`` should return a "variable length string", in reality it doesn't.

[changeomatic]: https://github.com/sixtyeight/changeomatic/blob/master/src/main/java/at/metalab/changeomatic/ChangeomaticMain.java 
[redis]: http://redis.io
[mep-rr]: https://en.wikipedia.org/wiki/Request%E2%80%93response
[mep-pubsub]: https://en.wikipedia.org/wiki/Publish%E2%80%93subscribe_pattern
[itl-ssp]: http://innovative-technology.com/product-files/ssp-manuals/smart-payout-ssp-manual.pdf
[itl-hw-hopper]: http://innovative-technology.com/products/products-main/210-smart-hopper
[itl-hw-validator]: http://innovative-technology.com/products/products-main/90-nv200


