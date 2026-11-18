// SA-MP particle effects in Grove Street, for docs/images/particle-preview.png.
#include <open.mp>

stock Haumea_particles_CreateObjects()
{
	CreateObject(18688, 2481.0, -1668.0, 12.0, 0.0, 0.0, 0.0); // fire
	CreateObject(18739, 2486.0, -1668.0, 12.4, 0.0, 0.0, 0.0); // water_fountain
	CreateObject(18717, 2491.0, -1668.0, 13.3, 0.0, 0.0, 0.0); // prt_spark
	CreateObject(18673, 2496.0, -1668.0, 11.8, 0.0, 0.0, 0.0); // cigarette_smoke
}
