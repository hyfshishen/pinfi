#!/usr/bin/python

import sys
import os
import getopt
import time
import random
import signal
import subprocess

currdir = "./"
progbin = currdir
pinbin = "pin"
instcategorylib = "../obj-intel64/instcategory.so"
instcountlib = "../obj-intel64/instcount.so"
filib = "../obj-intel64/faultinjection.so"
#inputfile = currdir + "/inputs/input.2048"
outputdir = currdir + "/std_output"
basedir = currdir + "/baseline"
errordir = currdir + "/error_output"
fileoutputdir = currdir + "/prog_output"
optionlist = []

if not os.path.isdir(outputdir):
  os.mkdir(outputdir)
if not os.path.isdir(basedir):
  os.mkdir(basedir)
if not os.path.isdir(errordir):
  os.mkdir(errordir)
if not os.path.isdir(fileoutputdir):
  os.mkdir(fileoutputdir)

timeout = 500

def execute(execlist):
	#print "Begin"
	#inputFile = open(inputfile, "r")
  global outputfile
  print ' '.join(execlist)
  #print outputfile
  outputFile = open(outputfile, "w")
  p = subprocess.Popen(execlist, stdout = outputFile)
  elapsetime = 0
  while (elapsetime < timeout):
    elapsetime += 1
    time.sleep(1)
    #print p.poll()
    if p.poll() is not None:
      print "\t program finish", p.returncode
      print "\t time taken", elapsetime
      #outputFile = open(outputfile, "w")
      #outputFile.write(p.communicate()[0])
      outputFile.close()
      #inputFile.close()
      return str(p.returncode)
  #inputFile.close()
  outputFile.close()
  print "\tParent : Child timed out. Cleaning up ... "
  p.kill()
  return "timed-out"
	#should never go here
  sys.exit(syscode)


def main():
  #clear previous output
  global run_number, optionlist, outputfile
  # outputfile = basedir + "/golden_output"
  # execlist = [pinbin, '-t', instcategorylib, '--', progbin]
  # execlist.extend(optionlist)
  # execute(execlist)


  # baseline
  filelist_bef1 = os.listdir('./')
  filecounter_bef1 = len(filelist_bef1)
  outputfile = basedir + "/golden_std_output"
  execlist = [pinbin, '-t', instcountlib, '--', progbin]
  execlist.extend(optionlist)
  execute(execlist)
  filelist_aft1 = os.listdir('./')
  filecounter_aft1 = len(filelist_aft1)
  if(filecounter_aft1 - filecounter_bef1 > 2):
    for item in filelist_aft1:
      if(item not in filelist_bef1 and item != "pin.instcount.txt" and item != "pintool.log" and item != "pin.log"):
        os.system("mv " + item + ' ' + basedir)

  # fault injection
  for index in range(0, run_number):
    filelist_bef2 = os.listdir('./')
    filecounter_bef2 = len(filelist_bef1)
    outputfile = outputdir + "/std_outputfile-" + str(index)
    errorfile = errordir + "/errorfile-" + str(index)
    execlist = [pinbin, '-t', filib, '-fioption', 'AllInst', '--', progbin]
    execlist.extend(optionlist)
    ret = execute(execlist)
    if ret == "timed-out":
      error_File = open(errorfile, 'w')
      error_File.write("Program hang\n")
      error_File.close()
    elif int(ret) < 0:
      error_File = open(errorfile, 'w')
      error_File.write("Program crashed, terminated by the system, return code " + ret + '\n')
      error_File.close()
    elif int(ret) > 0:
      error_File = open(errorfile, 'w')
      error_File.write("Program crashed, terminated by itself, return code " + ret + '\n')
      error_File.close()
    filelist_aft2 = os.listdir('./')
    filecounter_aft2 = len(filelist_aft2)
    for item in filelist_aft2:
      if(item not in filelist_bef2 and item != "activate"):
        fileoutputfile = fileoutputdir + '/' + item + '-' + str(index)
        os.system("mv " + item + ' ' + fileoutputfile)

if __name__=="__main__":
  global run_number
  assert len(sys.argv) == 4 and "Format: prog fi_number"
  progbin += sys.argv[1]
  run_number = int(sys.argv[2])
  optionlist = sys.argv[3].split(' ')
  main()
