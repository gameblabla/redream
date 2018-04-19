# GreedyDream

GreedyDream is basically me stealing Redream plus the numerous forks out there and combine it together into one thing.

Throw money at me, i love money ! I would do anything for money. 

And Sega please, buy my emulator so we can sell it and feature it as part of Sega forever !

However, you must rispect the GPLv3 license (see [LICENSE.txt](LICENSE.txt)) as well as the third party libraries that are each distributed under their own terms (see each library's license in [deps/](deps/)).

Don't ask questions ! Go ask your questions on Stackoverflow instead.

## Downloading

There are no pre-built binaries because i am homosexual and homosexuals prefer to build from source.

However for your conveniance, i have provided a Makefile as well. 

You can use it to cross-compile it. (although it won't be that useful with only a JIT for x86_64...)

## Building

Start by cloning the repository and setting up a build directory.

```
git clone https://github.com/gameblabla/redream.git
make -j7
```

Boom, no cmake crap.

However, if you are straight and uses cmake, do the following instead :

```
git clone https://github.com/gameblabla/redream.git
mkdir greedydream_build
cd greedydream_build
```

Next, generate a makefile or project file for your IDE of choice. For more info on the supported IDEs, checkout the [CMake documentation](http://www.cmake.org/cmake/help/latest/manual/cmake-generators.7.html).

```
# Makefile
cmake -DCMAKE_BUILD_TYPE=RELEASE ../greedydream

# Xcode project
cmake -G "Xcode" ../greedydream

# Visual Studio project
cmake -G "Visual Studio 14 Win64" ../greedydream
```

Only then you can type `make` from the command line or load up the project file and compile the code from inside of your IDE. (aka that piece of shit of Visual Studio)

## Reporting bugs

Do not report bugs because, again, i am homosexual and thus i am unable to fix bugs.

C'est la vie
