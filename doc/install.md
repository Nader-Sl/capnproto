---
layout: page
title: Installation
---

# Installation

<p style="font-size: 125%; font-weight: bold;">Note: Cap'n Proto is in BETA</p>

<div style="float: right"><a class="groups_link" style="color: #fff"
href="https://groups.google.com/group/capnproto-announce">Sign Up for Updates</a></div>

As of this writing, Cap'n Proto is in beta.  The serialization layer is close to feature-complete
and we don't anticipate any further changes to the wire format.  That said, if you want to use it,
you should keep in mind some caveats:

* **Security:** Cap'n Proto has not yet had a security review.  Although Kenton has a background
  in security and is not aware of any vulnerabilities in the current code, it's likely that there
  are a few.  For now, do not accept Cap'n Proto messages from parties you do not trust.
* **API Stability:** The Cap'n Proto programming interface may still change in ways that break
  existing code.  Such changes are likely to be minor and should not affect the wire format.
* **Performance:** While Cap'n Proto is inherently fast by design, the implementation has not yet
  undergone serious profiling and optimization.  Currently it only beats Protobufs in realistic-ish
  end-to-end benchmarks by around 2x-5x.  We can do better.
* **RPC:** The RPC protocol has not yet been specified, much less implemented.
* **Support for languages other than C++:** Work is being done to support languages other than C++,
  but at this time only the C++ implementation is ready to be used.

If you'd like to hack on Cap'n Proto, you should join the
[discussion group](https://groups.google.com/group/capnproto)!

If you'd just like to receive updates as things progress, add yourself to the
[announce list](https://groups.google.com/group/capnproto-announce).

## Installing the Cap'n Proto Compiler

`capnpc`, which takes `.capnp` files and generates source code for them (e.g. in C++), is itself
written in Haskell.

First, install [Cabal](http://www.haskell.org/cabal/), e.g. on Ubuntu:

    sudo apt-get install cabal-install

Now you can download and install the release tarball:

<pre><code>curl -O <a href="http://capnproto.org/capnproto-compiler-0.1.0.tar.gz">http://capnproto.org/capnproto-compiler-0.1.0.tar.gz</a>
cabal install capnproto-compiler-0.1.0.tar.gz</code></pre>

Be sure that the Cabal bin directory (e.g. `$HOME/.cabal/bin` on Ubuntu or
`$HOME/Library/Haskell/bin` on Mac OSX) is in your `PATH` before you attempt to build the C++
runtime.

### Building the compiler from Git mainline

If you want to try out the latest -- possibly broken! -- compiler, do:

    git clone https://github.com/kentonv/capnproto.git
    cd capnproto/compiler
    cabal install capnproto-compiler.cabal

## Installing the C++ Runtime

### GCC 4.7 or Clang 3.2 Needed

If you are using GCC, you MUST use at least version 4.7 as Cap'n Proto uses recently-implemented
C++11 features.  If GCC 4.7 is installed but your system's default GCC is older, you will probably
need to set the environment variable `CXX=g++-4.7` before following the instructions below.

If you are using Clang, you must use at least version 3.2.  To use Clang, set the environment
variable `CXX=clang++` before following any instructions below, otherwise `g++` is used by default.

##### Clang 3.2 on Mac OSX

As of this writing, Mac OSX 10.8 with Xcode 4.6 command-line tools is not quite good enough to
compile Cap'n Proto.  The included version of GCC is ancient.  The included version of Clang --
which mysteriously advertises itself as version 4.2 -- was actually cut from LLVM SVN somewhere
between versions 3.1 and 3.2; it is not sufficient to build Cap'n Proto.

There are two options:

1. Use [Macports](http://www.macports.org/) or [Fink](http://www.finkproject.org/) to get an
   up-to-date GCC.
2. Obtain Clang 3.2 (or better)
   [directly from the LLVM project](http://llvm.org/releases/download.html).

Option 2 is the one preferred by Cap'n Proto's developers.  Here are step-by-step instructions
for setting this up:

1. Get the Xcode command-line tools.  Download Xcode from the app store.  Then, open Xcode,
   go to Xcode menu > Preferences > Downloads, and choose to install "Command Line Tools".
2. Download the Clang 3.2 binaries and put them somewhere easy to remember:

       curl -O http://llvm.org/releases/3.2/clang+llvm-3.2-x86_64-apple-darwin11.tar.gz
       tar zxf clang+llvm-3.2-x86_64-apple-darwin11.tar.gz
       mv clang+llvm-3.2-x86_64-apple-darwin11 ~/clang-3.2

3. We will need to use libc++ (from LLVM) rather than libstdc++ (from GNU) because Xcode's
   libstdc++ (like its GCC) is too old.  In order for your freshly-downloaded Clang binaries to
   be able to find it, you'll need to symlink it into the Clang tree:

       ln -s /usr/lib/c++ ~/clang-3.2/lib/c++

You may now follow the instructions below, but make sure to tell `configure` to use your
newly-downloaded Clang binary:

    ./configure CXX=~/clang-3.2/bin/clang++

Hopefully, Xcode 5.0 will be released soon with a newer Clang, making this extra work unnecessary.

### Building from a release package

First, make sure you've installed the Cap'n Proto compiler, described above.  You MUST use the
exact same version of the compiler and the runtime library!  That means if you installed `capnpc`
from Git, you must install the runtime from Git.

You may download and install the release version of the C++ runtime like so:

<pre><code>curl -O <a href="http://capnproto.org/capnproto-c++-0.1.0.tar.gz">http://capnproto.org/capnproto-c++-0.1.0.tar.gz</a>
tar zxf capnproto-c++-0.1.0.tar.gz
cd capnproto-c++-0.1.0
./configure
make -j6 check
sudo make install</code></pre>

This will install `libcapnp` in `/usr/local/lib` and headers in `/usr/local/include/capnp` and
`/usr/local/include/kj`.

### Building from Git with Autotools

If you download directly from Git, and you don't want to
[build with Ekam](install.html#building_with_ekam), you will need to have the GNU autotools --
[autoconf](http://www.gnu.org/software/autoconf/),
[automake](http://www.gnu.org/software/automake/), and
[libtool](http://www.gnu.org/software/libtool/) -- installed.  You will also need Subversion
installed (in addition to Git) in order to fetch the Google Test sources (done by
`setup-autotools.sh`).

    git clone https://github.com/kentonv/capnproto.git
    cd capnproto/c++
    ./setup-autotools.sh
    autoreconf -i
    ./configure
    make -j6 check
    sudo make install

### Building with Ekam

Ekam is a build system I wrote a while back that automatically figures out how to build your C++
code without instructions.  It also supports continuous builds, where it watches the filesystem for
changes (via inotify) and immediately rebuilds as necessary.  Instant feedback is key to
productivity, so I really like using Ekam.

Unfortunately it's very much unfinished.  It works (for me), but it is quirky and rough around the
edges.  It only works on Linux, and is best used together with Eclipse.  If you find it
unacceptable, scroll up to the [Automake instructions](#building_from_git_with_autotools).

The Cap'n Proto repo includes a script which will attempt to set up Ekam for you.

    git clone https://github.com/kentonv/capnproto.git
    cd capnproto/c++
    ./setup-ekam.sh

If all goes well, this downloads the Ekam code into a directory called `.ekam` and adds some
symlinks under src.  It also imports the [Google Test](https://googletest.googlecode.com) and
[Protobuf](http://protobuf.googlecode.com) source code, so you can compile tests and benchmarks.

Once Ekam is installed, you can do:

    make -f Makefile.ekam once

This will build everything it can and run tests.

Note that Ekam will fail to build some things and output a bunch of error messages.  You should
be able to ignore any errors that originate outside of the `capnp` and `kj` directories -- these
are just parts of other packages like Google Test that Ekam doesn't fully know how to build, but
aren't needed by Cap'n Proto anyway.

#### Running the Benchmarks

Before getting into benchmarks, let me be frank:  performance varies wildly by use case, and no
benchmark is going to properly reflect the big picture.  If performance is critical to your use
case, you should write a benchmark specific to your case, and test multiple serialization
technologies.  Don't assume anything.  If you find Cap'n Proto performs sub-optimally, though,
[tell us about it](https://groups.google.com/group/capnproto).

That said, Cap'n Proto does have a small suite of silly benchmarks used to validate changes.

The Ekam build will put the benchmark binaries in `tmp/capnp/benchmark`.

    tmp/capnp/benchmark/runner

This runs the default test case, CatRank.  CatRank simulates a search engine scoring algorithm
which promotes pages that discuss cats (and demotes ones discussing dogs).  A list of up to 1000
random search results with URLs, scores, and snippets is sent to the server, which searches the
snippets for instances of "cat" and "dog", adjusts their scores accordingly, then returns the new
result list sorted by score.

This test case is very string-heavy.  Cap'n Proto performs well due to its zero-copy strings, but
packing the message doesn't help much.

    tmp/capnp/benchmark/runner eval

In this test case, the client generates a random, deeply-nested arithmetic expression for the
server to evaluate.  This case is a pathologically bad case for Cap'n Proto as it involves lots of
pointers with relatively little actual data.  When packing is enabled it actually loses to
Protobufs by a little bit on CPU time (as of this writing, at least; it'll probably get better with
optimization).

    tmp/capnp/benchmark/runner carsales

This test case involves sending to the server a description of a bunch of cars, and asks the server
to decide how much the lot is worth.  This case is very number-heavy, and because of this
Cap'n Proto's "packed" mode really shines.

#### Developing with Ekam

If you intend to do some development, you should build `continuous` or `continuous-opt` instead
of `once`.  These modes will build everything, then watch the source tree for changes and rebuild
as necessary.  `continuous` does a debug build while `continuous-opt` optimizes; the former is best
while developing but don't run the benchmarks in debug mode!

If you use Eclipse, you should use the Ekam Eclipse plugin to get build results fed back into your
editor while building in continuous mode.  Build the plugin like so:

1. Open the `.ekam/eclipse` directory as an Eclipse project.
2. File -> Export -> Plug-in Development -> Deployable Plug-ins and Fragments.
3. Choose the Ekam Dashboard project and export to your Eclipse directory, or export to another
   directory and copy the files into your Eclipse directory.
4. Restart Eclipse.
5. Make sure you have some sort of project in your work space containing your Ekam source tree. It
   should be rooted at the directory containing "src", "tmp", etc. The plugin will mark errors
   within this project.
6. Window -> Show View -> Other -> Ekam -> Ekam Dashboard

The dashboard view lets you browse the whole tree of build actions and also populates your editor
with error markers.