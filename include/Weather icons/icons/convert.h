#pragma once
#include <Arduino.h>
// snow	Amount of snow is greater than zero
// snow-showers-day	Periods of snow during the day
// snow-showers-night	Periods of snow during the night
// thunder-rain	Thunderstorms throughout the day or night
// thunder-showers-day	Possible thunderstorms throughout the day
// thunder-showers-night	Possible thunderstorms throughout the night
// rain	Amount of rainfall is greater than zero
// showers-day	Rain showers during the day
// showers-night	Rain showers during the night
// fog	Visibility is low (lower than one kilometer or mile)
// wind	Wind speed is high (greater than 30 kph or mph)
// cloudy	Cloud cover is greater than 90% cover
// partly-cloudy-day	Cloud cover is greater than 20% cover during day time.
// partly-cloudy-night	Cloud cover is greater than 20% cover during night time.
// clear-day	Cloud cover is less than 20% cover during day time
// clear-night	Cloud cover is less than 20% cover during night time

/// @brief Converts a visual crossing icon to a weather icon ID
/// @param icon The icon in the visual crossing api response
/// @return the ID of the icon used in the weather_icons.h
int visualCrossingToId (String icon)
{
  if (icon == "snow") return 15; // w_snowfall_and_blue_cloud_16541
  if (icon == "snow-showers-day") return 20; // w_yellow_sun_and_snow_with_blue_cloud_16542
  if (icon == "snow-showers-night") return 3; // w_blue_moon_and_snowy_night_16543
  if (icon == "thunder-rain") return 9; // w_lightning_and_blue_rain_cloud_16533
  if (icon == "thunder-showers-day") return 6; // w_cloud_and_yellow_lightning_16534
  if (icon == "thunder-showers-night") return 6;// w_cloud_and_yellow_lightning_16534
  if (icon == "rain") return 7; // w_downpour_rainy_day_16531
  if (icon == "showers-day") return 19; // w_yellow_sun_and_blue_rain_cloud_16535
  if (icon == "showers-night") return 13; // w_rainy_night_and_clouds_with_moon_16539
  if (icon == "fog") return -1; // not defined
  if (icon == "wind") return -2; // not defined
  if (icon == "cloudy") return 0; //w_blue_cloud_and_weather_16527
  if (icon == "partly-cloudy-day") return 2; //w_yellow_sun_and_blue_cloud_16528
  if (icon == "partly-cloudy-night") return 1; // w_blue_clouds_and_blue_moon_16538
  if (icon == "clear-day") return 17; // w_yellow_sun_16526
  if (icon == "clear-night") return 16;// w_yellow_moon_16536
  return -3;
}