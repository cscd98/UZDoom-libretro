#pragma once

#include "retro_context.h"

// Android's JNI requires frontend callbacks to run on the thread's native stack,
// not the engine's alternate stack.
class RetroFrontendStack
{
    struct Work
    {
        void (*function)(void *);
        void *argument;
    };

    RetroContext &frontend;
    RetroContext &engine;
    Work *pending = nullptr;
    inline static thread_local RetroFrontendStack *active = nullptr;

public:
    RetroFrontendStack(RetroContext &frontend, RetroContext &engine)
        : frontend(frontend), engine(engine) {}

    void Resume()
    {
        do
        {
            auto previous = active;
            active = this;
            SwitchContext(&frontend, &engine);
            active = previous;

            if (!pending)
                break;

            auto work = pending;
            pending = nullptr;
            work->function(work->argument);
        } while (true);
    }

    static void Call(void (*function)(void *), void *argument)
    {
        if (!active)
        {
            function(argument);
            return;
        }

        Work work{function, argument};
        auto dispatcher = active;
        dispatcher->pending = &work;
        SwitchContext(&dispatcher->engine, &dispatcher->frontend);
    }
};
