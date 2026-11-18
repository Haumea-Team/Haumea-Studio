// SA-MP tubes (19646, TubeStraight 10 m) over the sea, to join more to:
// one along X and one turned to run along Y.
#include <a_samp>

stock UiTubes()
{
	CreateObject(19646, 2000.000000, -3000.000000, 20.000000, 0.000000, 0.000000, 0.000000);
	CreateObject(19646, 2100.000000, -3000.000000, 20.000000, 0.000000, 0.000000, 90.000000);
}
