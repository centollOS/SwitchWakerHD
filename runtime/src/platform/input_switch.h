#pragma once
namespace input {
// Main thread (Switch host loop): read the controllers, show a pending software keyboard.
void update();
void stop_rumble();  // the motors still (the host loop ends)
}
