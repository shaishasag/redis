This is my attempt to translate Redis to C++. It is not meant to criticise redis or to "improve" it. Redis is implemented in C and is perfectly fine as it is.

So why translate redis to C++?
I love redis and wanted to learn it's internal implementation, and what a better way for a programmer to learn someone else's code than to rewrite it in a familiar language?

What I did so far:
* renamed all .c files under src directory to .cpp
* changed makefile to compile .cpp files as C++
* compiled everything and fixed many compilation error due to differences between C and C++. 
  C++ is more strict and requires a cast when assigning on type of pointer to another. So
`dict* d = o->ptr`
becomes:
`dict* d = (dict*)o->ptr`
* converted many declarations with the cumbersome syntax like:

    `typedef struct geoPoint {...} geoPoint;`

    to the simpler:

    `struct geoPoint {...};`

    (I Never understood the need to "typedef" each and every struct declaration and the double naming both before and after the curly braces.) 

* Converted the structs is dict.h to classes. Many global functions became member functions. Some members are private and accessed through accessor methods.

What is Redis?
--------------
The original redis project code is [here](https://github.com/antirez/redis)
You can find more detailed documentation of redis at [redis.io](https://redis.io).
