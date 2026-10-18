package main

import (
	"strconv"
	"strings"
	"time"
)

type adbDevice struct {
	serial string
	state  string // device, unauthorized, offline, authorizing, "no permissions", ...
	model  string // from "model:Quest_3" in devices -l
}

// parseDevices reads the output of "adb devices -l".
func parseDevices(out string) []adbDevice {
	var devs []adbDevice
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "List of devices") || strings.HasPrefix(line, "*") || strings.HasPrefix(line, "adb ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		d := adbDevice{serial: fields[0], state: fields[1]}
		if d.state == "no" && len(fields) > 2 && fields[2] == "permissions" {
			d.state = "no permissions"
		}
		for _, f := range fields[2:] {
			if strings.HasPrefix(f, "model:") {
				d.model = strings.ReplaceAll(strings.TrimPrefix(f, "model:"), "_", " ")
			}
		}
		devs = append(devs, d)
	}
	return devs
}

// waitMessage tells the user what to do for the current device states.
func waitMessage(devs []adbDevice) string {
	has := func(state string) bool {
		for _, d := range devs {
			if d.state == state {
				return true
			}
		}
		return false
	}
	switch {
	case has("unauthorized"):
		return "Put on the headset and choose Allow (tick \"Always allow from this computer\")."
	case has("no permissions"):
		return "adb is not allowed to use the headset. On Linux, set up the udev rules for\n" +
			"Android devices (or run adb as your user with the right group), then reconnect."
	case has("offline"), has("authorizing"), has("connecting"):
		return "The headset is connecting... please wait."
	case len(devs) > 0:
		return "The headset is busy (" + devs[0].state + "). Please wait or restart the headset."
	default:
		return "Connect the headset with a USB-C cable and put it on.\n" +
			"(Developer mode must be on for the headset. See README.txt.)"
	}
}

// deviceDisplayName turns getprop values into "Meta Quest 3", "PICO 4", ...
func deviceDisplayName(manufacturer, model string) string {
	manufacturer = strings.TrimSpace(manufacturer)
	model = strings.TrimSpace(model)
	if model == "" {
		if manufacturer == "" {
			return "headset"
		}
		return manufacturer + " headset"
	}
	brand := manufacturer
	lb := strings.ToLower(brand)
	if lb == "oculus" || lb == "meta" || strings.Contains(lb, "facebook") {
		brand = "Meta"
	}
	lm := strings.ToLower(model)
	if brand == "" || strings.HasPrefix(lm, strings.ToLower(brand)) || strings.HasPrefix(lm, "oculus") || strings.HasPrefix(lm, "meta") {
		return model
	}
	return brand + " " + model
}

// connect is step 3: wait until exactly one headset is connected and allowed.
func (a *app) connect() error {
	if _, err := a.adb.run(60*time.Second, "start-server"); err != nil {
		say("adb had trouble starting; trying anyway.")
	}
	last := ""
	shownHint := false
	failures := 0
	for {
		out, err := a.adb.run(20*time.Second, "devices", "-l")
		if err != nil {
			failures++
			if failures == 5 {
				say("adb does not answer. If another program uses adb (SideQuest, Meta Quest")
				say("Developer Hub, Android Studio), close it and run the launcher again.")
			}
			time.Sleep(2 * time.Second)
			continue
		}
		devs := parseDevices(out)
		var ready []adbDevice
		for _, d := range devs {
			if d.state == "device" {
				ready = append(ready, d)
			}
		}
		if len(ready) == 1 {
			a.adb.Serial = ready[0].serial
			break
		}
		if len(ready) > 1 {
			say("Several devices are connected:")
			for i, d := range ready {
				name := d.model
				if name == "" {
					name = "device"
				}
				say("  %d) %s (%s)", i+1, name, d.serial)
			}
			choice := 1
			for {
				ans, ok := a.ui.ask("Type the number of your headset:")
				if !ok {
					break
				}
				if n, err := strconv.Atoi(ans); err == nil && n >= 1 && n <= len(ready) {
					choice = n
					break
				}
			}
			a.adb.Serial = ready[choice-1].serial
			break
		}
		msg := waitMessage(devs)
		if msg != last {
			say("%s", msg)
			if !shownHint {
				say("(Waiting for the headset. Press Ctrl+C to stop.)")
				shownHint = true
			}
			last = msg
		}
		time.Sleep(2 * time.Second)
	}

	model, _ := a.adb.shell(20*time.Second, "getprop ro.product.model")
	manu, _ := a.adb.shell(20*time.Second, "getprop ro.product.manufacturer")
	say("Found: %s", deviceDisplayName(firstLine(manu), firstLine(model)))
	return nil
}

func firstLine(s string) string {
	s = strings.TrimSpace(s)
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	return strings.TrimSpace(s)
}
