! The flang binary drives Fortran compilations in the flang mode even when it
! is installed under a name without the "flang" suffix, such as gfortran.

! UNSUPPORTED: system-windows

! RUN: rm -rf %t && mkdir -p %t
! RUN: ln -s %flang %t/gfortran
! RUN: %t/gfortran -### -c %s 2>&1 | FileCheck %s
! RUN: %t/gfortran --driver-mode=flang -### -c %s 2>&1 | FileCheck %s

! CHECK: "-fc1"
! CHECK-NOT: "-cc1"

end
