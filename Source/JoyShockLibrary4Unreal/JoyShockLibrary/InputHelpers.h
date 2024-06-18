#pragma once
#include "JoyShockLibrary.h"

class JoyShock;

bool handle_input(JoyShock* jc, uint8_t* packet, int32 len, bool &hasIMU);
