# windowManager
simple dynamic tiling window manager for Xorg

# Usage
1. Clone the repository
```
git clone https://github.com/kuglatec/windowManager
```
2. Install Xephyr for using a nested X session
3. Install libx11
4. install google glog
5. install xterm (idk why but it wont work without it being started by xinitrc)
6. install rofi (application launcher)
7. ```cd windowManager```
8. ```./buiild_and_run.sh```



# Keybindings

```Alt + LeftMouseButton``` | move window  


```Alt + RightMouseButton```| resize window  


```Alt + Return```| open rofi  


```Alt + Right``` | extend to right (tiling mode)  


```Alt + Left``` | shrink to right (tiling mode)  

```Alt + d``` | swap window right (tiling mode)  

```Alt + a``` | swap window left (tiling mode)  

```Alt + t``` | enter tiling mode (allign windows)
# credits
jichu4n for his blog series about x window managers and his basic_wm which this project is based off 

# Features

allthough this project is barely usable and unstable, you can get a overview of features included (features marked with N will be included in the next release)

floating Windows | Y


vertical tiling | Y


gaps | N


resizing windows | Y


swapping windows | Y


EWMH bar support (e.g. polybar) | Y


configuration support | N
