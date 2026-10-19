# openHAB items for the example

The items [`demo/config.json`](../demo/config.json) names (the configuration the
[demo](https://bruestel.github.io/ohpanel/demo/) shows), as one `.items` file. The panel only needs the items:
how they get their states (a binding's channel, a rule, a profile) is up to you. The channels below show what usually
feeds them; `<...>` stands for your own Thing. Items made in the Main UI work the same.

What the panel reads of an item: its state, its type and its formatted value (`displayState`: the pattern, a MAP
transformation or an option label), and for some pages its state description (a slider's minimum and maximum) or its
metadata. Mostly the configuration names what each field says; a group's rows (an entities or a cover page with
`group`) show the members' labels.

```
// --- Home screen ------------------------------------------------------------------------------------------------

// the two relays: a plain Switch each, no channel -- the panel keeps it in step with its relay
// (home.relays[].item; "relay": false on the second: the button switches the item only, not the relay)
Switch Kitchen_Ceiling             "Kitchen ceiling light"
Switch Kitchen_Counter_Switch      "Counter light"                   { channel="<light>:switch" }

// weather: outdoor temperature, today's low and high, the condition's icon code (home.weather_icon, "codes")
Number:Temperature Weather_Temperature "Outdoor temperature [%.1f %unit%]" { channel="openweathermap:onecall:<api>:<place>:current#temperature" }
Number:Temperature Weather_Today_Min   "Today low [%.1f %unit%]"     { channel="openweathermap:onecall:<api>:<place>:forecastToday#min-temperature" }
Number:Temperature Weather_Today_Max   "Today high [%.1f %unit%]"    { channel="openweathermap:onecall:<api>:<place>:forecastToday#max-temperature" }
String             Weather_Icon        "Weather icon"                { channel="openweathermap:onecall:<api>:<place>:current#icon-id" }

// the two value rows
Number:Dimensionless Kitchen_Humidity    "Kitchen humidity [%d %%]"         { channel="<sensor>:humidity" }
Number:Temperature   Kitchen_Temperature "Kitchen temperature [%.1f %unit%]" { channel="<sensor>:temperature" }

// status icons and shortcuts: shown while ON (or OPEN, PLAY, a number above 0, or the states of their "on")
Switch  Plants_Warning       "A plant needs water"           // set by a rule over the plant sensors
Contact Window_Bath          "Bathroom window"               { channel="<contact>:contact" }
String  Speaker_State        "Kitchen speaker state"         { channel="sonos:<player>:<id>:state" }   // "on": "PLAYING"
String  Washer_State         "Washer state"                  { channel="<washer>:state" }
Switch  Waste_Paper_Alarm    "Paper collection tomorrow"     // a rule: ON from the evening before until the morning of
Switch  Waste_Organic_Alarm  "Organic collection tomorrow"

// the two info lines: line, destination, departure (left, main, right) -- from a public transport binding or a rule
String Bus1_Line             "Next bus line"
String Bus1_Destination      "Next bus destination"
String Bus1_Departure        "Next bus departure"
String Bus2_Line             "Second bus line"
String Bus2_Destination      "Second bus destination"
String Bus2_Departure        "Second bus departure"

// --- Light (type "light") -------------------------------------------------------------------------------------

Dimmer             Kitchen_Counter_Brightness "Counter light brightness" { channel="<light>:brightness" }
// in K: the slider runs from the state description's minimum to its maximum, else 2000 to 6500 K
Number:Temperature Kitchen_Counter_ColorTemp  "Counter light colour temperature [%d K]" { channel="<light>:color-temperature-abs" }
Color              Kitchen_Counter_Color      "Counter light colour"     { channel="<light>:color" }

// --- Thermostat (type "thermostat") ---------------------------------------------------------------------------

Number:Temperature Thermostat_Kitchen_Setpoint "Kitchen target [%.1f %unit%]" { channel="<thermostat>:setpoint",
                                                  stateDescription=" " [min=5, max=30, step=0.5] }
Switch             Thermostat_Kitchen_Heating  "Kitchen heating"         { channel="<thermostat>:heating" }
String             Thermostat_Kitchen_Mode     "Kitchen heating mode"    { channel="<thermostat>:mode" }   // auto, manual, off

// --- Media player (type "media") and the station list (type "media_favorites") ------------------------------
// the channels of the Sonos binding; other players have their own names for the same

Player Speaker_Control       "Kitchen speaker"              { channel="sonos:<player>:<id>:control" }
Dimmer Speaker_Volume        "Kitchen speaker volume"       { channel="sonos:<player>:<id>:volume" }
Switch Speaker_Mute          "Kitchen speaker mute"         { channel="sonos:<player>:<id>:mute" }
String Speaker_Title         "Title"                        { channel="sonos:<player>:<id>:currenttitle" }
String Speaker_Artist        "Artist"                       { channel="sonos:<player>:<id>:currentartist" }
String Speaker_Album         "Album"                        { channel="sonos:<player>:<id>:currentalbum" }
String Speaker_PlayUri       "Play URI"                     { channel="sonos:<player>:<id>:playuri" }

// the station catalogue: no channel, no rule -- its command options are the stations (stream URI=key), its
// "ohpanel" metadata their names, short names, icons and logos (docs/panel-config.md, media_favorites)
String Radio_Stations "Radio stations" {
    commandDescription=" " [options="x-rincon-mp3radio://radio.example/one.m3u=one,x-rincon-mp3radio://radio.example/news.m3u=news"],
    ohpanel=" " [stations="{
        \"one\": {\"name\": \"Radio One\", \"short\": \"One\", \"icon\": \"radio\"},
        \"news\": {\"name\": \"News Radio\", \"short\": \"News\", \"icon\": \"newspaper-variant-outline\"}
    }"]}

// --- Covers (type "cover") --------------------------------------------------------------------------------------
// 0 % open, 100 % closed (most bindings; "reverse": true for the other way round); the "cover" metadata gives what
// the page leaves out: style, presets, a label

Rollershutter Blind_Living_Left  "Living room left"   (gBlinds, gBlinds_Front) { channel="<blind>:position" }
Rollershutter Blind_Living_Right "Living room right"  (gBlinds, gBlinds_Front) { channel="<blind>:position" }
Rollershutter Blind_Kitchen      "Kitchen"            (gBlinds, gBlinds_Front) { channel="<blind>:position" }
Rollershutter Blind_Bedroom      "Bedroom"            (gBlinds)                { channel="<blind>:position" }
Rollershutter Awning_Terrace     "Terrace awning"     (gBlinds)                { channel="<awning>:position",
                                                                                 cover=" " [style="awning", presets="Sun=60, Shade=100"] }

// a group page shows the members as rows, the group itself on top: with a state (AVG) its position, with presets
// pills that move every member
Group:Rollershutter:AVG gBlinds_Front "Front sun protection" { cover=" " [presets="50 %=50, 65 %=65, 75 %=75"] }
Group:Rollershutter:AVG gBlinds       "All blinds"

// --- Lists of values (type "entities") ------------------------------------------------------------------------

// energy
Number:Power  Grid_Power        "Grid [%d W]"            { channel="<meter>:power" }        // below 0: export
Number:Power  Solar_Power       "Solar [%d W]"           { channel="<inverter>:power" }
Number:Dimensionless Battery_Level "Battery [%d %%]"     { channel="<battery>:level" }
Number:Power  House_Power       "House [%d W]"
Number:Energy Grid_Import_Today "Import today [%.1f kWh]"
Number:Energy Grid_Export_Today "Export today [%.1f kWh]"
Number:Energy Solar_Today       "Solar today [%.1f kWh]"
Number:Energy Solar_Yesterday   "Solar yesterday [%.1f kWh]"

// plants: soil moisture each; the page's rules colour them (at most 20 red, 30 yellow) -- or, with a group and
// "plant" metadata, by each one's own thresholds (docs/panel-config.md, entities with metadata)
Number:Dimensionless Plant_Ficus    "Ficus soil moisture [%d %%]"      { channel="<sensor>:moisture" }
Number:Dimensionless Plant_Monstera "Monstera soil moisture [%d %%]"   { channel="<sensor>:moisture" }
Number:Dimensionless Plant_Basil    "Basil soil moisture [%d %%]"      { channel="<sensor>:moisture" }
Number:Dimensionless Plant_Orchid   "Orchid soil moisture [%d %%]"     { channel="<sensor>:moisture" }
Number:Dimensionless Plant_Lemon    "Lemon tree soil moisture [%d %%]" { channel="<sensor>:moisture" }

// waste collection: the next date of each bin (a DateTime, e.g. from the iCalendar binding or a rule); the page
// colours one yellow from 17:00 the evening before until 08:00 on the day ("within_days": 1, "from", "until")
DateTime Waste_Residual_Next "Residual waste [%1$ta, %1$td.%1$tm.]"
DateTime Waste_Organic_Next  "Organic waste [%1$ta, %1$td.%1$tm.]"
DateTime Waste_Plastic_Next  "Plastic [%1$ta, %1$td.%1$tm.]"
DateTime Waste_Paper_Next    "Paper [%1$ta, %1$td.%1$tm.]"

// the washing machine
String               Washer_Program   "Washer program"        { channel="<washer>:program" }
Number:Dimensionless Washer_Progress  "Washer progress [%d %%]" { channel="<washer>:progress" }
String               Washer_Remaining "Washer remaining"      { channel="<washer>:remaining" }
String               Washer_Load      "Washer load"           { channel="<washer>:load" }

// the dishwasher, on a button of the kitchen page
String Dishwasher_State "Dishwasher state" { channel="<dishwasher>:state" }

// --- Weather (type "weather") ---------------------------------------------------------------------------------
// a day each: icon code, low, high, the condition in words, and the extras (wind, rain, UV)

String             Weather_Today_Condition    "Today"         { channel="openweathermap:onecall:<api>:<place>:forecastToday#condition" }
Number:Speed       Weather_Today_Wind         "Wind today [%d km/h]"  { channel="openweathermap:onecall:<api>:<place>:forecastToday#wind-speed" }
Number:Length      Weather_Today_Rain         "Rain today [%.1f mm]"  { channel="openweathermap:onecall:<api>:<place>:forecastToday#rain" }
Number             Weather_Today_UV           "UV today"      { channel="openweathermap:onecall:<api>:<place>:forecastToday#uvindex" }
String             Weather_Tomorrow_Icon      "Weather icon tomorrow" { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#icon-id" }
Number:Temperature Weather_Tomorrow_Min       "Tomorrow low [%.1f %unit%]"  { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#min-temperature" }
Number:Temperature Weather_Tomorrow_Max       "Tomorrow high [%.1f %unit%]" { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#max-temperature" }
String             Weather_Tomorrow_Condition "Tomorrow"      { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#condition" }
Number:Speed       Weather_Tomorrow_Wind      "Wind tomorrow [%d km/h]"     { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#wind-speed" }
Number:Length      Weather_Tomorrow_Rain      "Rain tomorrow [%.1f mm]"     { channel="openweathermap:onecall:<api>:<place>:forecastTomorrow#rain" }
String             Weather_Day2_Icon          "Weather icon in two days"    { channel="openweathermap:onecall:<api>:<place>:forecastDay2#icon-id" }
Number:Temperature Weather_Day2_Min           "Low in two days [%.1f %unit%]"  { channel="openweathermap:onecall:<api>:<place>:forecastDay2#min-temperature" }
Number:Temperature Weather_Day2_Max           "High in two days [%.1f %unit%]" { channel="openweathermap:onecall:<api>:<place>:forecastDay2#max-temperature" }
String             Weather_Day2_Condition     "In two days"   { channel="openweathermap:onecall:<api>:<place>:forecastDay2#condition" }
```

Which icon each weather code shows, for OpenWeatherMap and other weather add-ons:
[Weather codes and icons](weather-codes.md).
