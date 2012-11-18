slurp
=====

slurp is a super simple C utility that, well, slurps down tweets from Twitter's sample stream. It was meant more as a proof-of-concept than anything else, so I would *not* recommend running this code in anything approaching a production scenario. However, hopefully it will be used to 1) give developers an idea of how to work with HTTP streams using libcurl (though again, not guaranteeing my approach is even close to the best one, copy at your own risk), and 2) help people who just want a simple way to download tons of raw Twitter data.

Building
========

Run
	make

libcurl and liboauth are required.

