package twrp

import (
	"android/soong/android"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path"
	"strings"
)

func getRecoveryAbsDir(ctx android.BaseContext) string {
	return getBuildAbsDir(ctx) + "bootable/recovery/"
}

func getBuildAbsDir(ctx android.BaseContext) string {
	outDir := ctx.Config().Getenv("OUT")
	absIndex := strings.Index(outDir, "out")
	return string(outDir[0:absIndex])
}

func copyDir(src string, dest string) error {
	var err error
	var fds []os.FileInfo
	var srcinfo os.FileInfo

	if srcinfo, err = os.Stat(src); err != nil {
		return err
	}

	if err = os.MkdirAll(dest, srcinfo.Mode()); err != nil {
		return err
	}

	if fds, err = ioutil.ReadDir(src); err != nil {
		return err
	}
	for _, fd := range fds {
		srcfp := path.Join(src, fd.Name())
		dstfp := path.Join(dest, fd.Name())

		if fd.IsDir() {
			if err = copyDir(srcfp, dstfp); err != nil {
				fmt.Println(err)
			}
		} else {
			if err = copyFile(srcfp, dstfp); err != nil {
				fmt.Println(err)
			}
		}
	}
	return nil
}

func copyFile(src string, dest string) error {
	var err error
	var srcfd *os.File
	var dstfd *os.File
	var srcinfo os.FileInfo

	if srcfd, err = os.Open(src); err != nil {
		return err
	}
	defer srcfd.Close()

	if dstfd, err = os.Create(dest); err != nil {
		return err
	}
	defer dstfd.Close()

	if _, err = io.Copy(dstfd, srcfd); err != nil {
		return err
	}
	if srcinfo, err = os.Stat(src); err != nil {
		return err
	}
	return os.Chmod(dest, srcinfo.Mode())
}
