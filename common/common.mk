# RTBKIT common makefile

LIBBIDREQUEST_SOURCES := \
	bid_request.cc \
	segments.cc \
	json_holder.cc \
	currency.cc \

LIBBIDREQUEST_LINK := \
	types boost_regex db

$(eval $(call library,bid_request,$(LIBBIDREQUEST_SOURCES),$(LIBBIDREQUEST_LINK)))

LIBRTB_SOURCES := \
	auction.cc \
	augmentation.cc \
	account_key.cc

LIBRTB_LINK := \
	ACE arch utils jsoncpp boost_thread endpoint boost_regex zmq opstats bid_request

$(eval $(call library,rtb,$(LIBRTB_SOURCES),$(LIBRTB_LINK)))
