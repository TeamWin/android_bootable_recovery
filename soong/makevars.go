package twrp

import (
	"android/soong/android"
)

func getMakeVars(ctx android.BaseContext, mVar string) string {
	makeVars := ctx.Config().VendorConfig("makeVarsPlugin")
	var makeVar = ""
	if makeVars.IsSet(mVar) {
		makeVar = makeVars.String(mVar)
	}
	return makeVar
}
