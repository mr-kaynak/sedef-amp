// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' main: JUCE starts before the first test and shuts down when the tests are done, while
// every static object is still alive. Left to static destruction at exit, JUCE's shutdown deletes
// its MIDI endpoint list after statics it uses are gone, and the program aborts on its way out.
#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

int main (int argc, char* argv[])
{
    int result = 0;
    {
        const juce::ScopedJuceInitialiser_GUI runtime;
        result = Catch::Session().run (argc, argv);
    }
    return result;
}
