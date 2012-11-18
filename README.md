Summary
=======

slurp is a super simple C utility that, well, slurps down tweets from Twitter's sample stream. It was meant more as a proof-of-concept than anything else, so I would *not* recommend running this code in anything approaching a production scenario. However, hopefully it will be used to 1) give developers an idea of how to work with HTTP streams using libcurl (though again, not guaranteeing my approach is even close to the best one, copy at your own risk), and 2) help people who just want a simple way to download tons of raw Twitter data.

Building
========

Run make. libcurl and liboauth are required.

Running
=======

./slurp keys.txt [optional output file]

keys.txt (or whatever you choose to call it) is a plain text file containing your Twitter OAuth app information, in this order:
Consumer key
Consumer secret
Access token
Access token secret

If you don't have this stuff, log into the Twitter dev center and create a new application.
