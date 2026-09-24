import datetime
import os
import string
import re
Import("env")

print("Current CLI targets", COMMAND_LINE_TARGETS)
print("Current Build targets", BUILD_TARGETS)

#VERSION_FOLDER = 'aaD:\Projects\PIO\Adler\Adler-IR\src\Version/'
VERSION_FOLDER = env.subst("$PROJECT_DIR") + os.sep + "src" + os.sep + "Version" + os.sep
VERSION_FILE = VERSION_FOLDER + 'version'
VERSION_HEADER = 'Version.h'
VERSION_PREFIX = '0.1.'
VERSION_PATCH_NUMBER = 0

if not os.path.exists(VERSION_FOLDER + ".version_no_increment"):
    try:
        with open(VERSION_FILE) as FILE:
            VERSION_PATCH_NUMBER = FILE.readline()
            VERSION_PREFIX = VERSION_PATCH_NUMBER[0:VERSION_PATCH_NUMBER.rindex('.')+1]
            VERSION_PATCH_NUMBER = int(VERSION_PATCH_NUMBER[VERSION_PATCH_NUMBER.rindex('.')+1:])
            if not os.path.exists(VERSION_FOLDER +".version_no_increment_update_date"):
                VERSION_PATCH_NUMBER = VERSION_PATCH_NUMBER + 1
    except:
        print('No version file found or incorrect data in it. Starting from 0.1.0')
        VERSION_PATCH_NUMBER = 0
    with open(VERSION_FILE, 'w+') as FILE:
        FILE.write(VERSION_PREFIX + str(VERSION_PATCH_NUMBER))
        print('Build number: {}'.format(VERSION_PREFIX + str(VERSION_PATCH_NUMBER)))

    HEADER_FILE = """
    // AUTO GENERATED FILE, DO NOT EDIT
    #ifndef VERSION
        #define VERSION "{}"
    #endif
    #ifndef BUILD_TIMESTAMP
        #define BUILD_TIMESTAMP "{}"
    #endif
    """.format(VERSION_PREFIX + str(VERSION_PATCH_NUMBER), datetime.datetime.now())

    if os.environ.get('PLATFORMIO_INCLUDE_DIR') is not None:
        VERSION_HEADER = os.environ.get('PLATFORMIO_INCLUDE_DIR') + os.sep + VERSION_HEADER
    elif os.path.exists("include"):
        VERSION_HEADER = "include" + os.sep + VERSION_HEADER
    else:
        PROJECT_DIR = env.subst("$PROJECT_DIR")
        os.mkdir(PROJECT_DIR + os.sep + "include")
        VERSION_HEADER = "include" + os.sep + VERSION_HEADER

    with open(VERSION_HEADER, 'w+') as FILE:
        FILE.write(HEADER_FILE)

    open(VERSION_FOLDER + '.version_no_increment', 'a').close()
else:
    if os.path.exists(VERSION_FOLDER + "version"):
        FILE = open(VERSION_FILE)
        VERSION_NUMBER = FILE.readline()
        print('Build number: {} (waiting for upload before next increment)'.format(str(VERSION_NUMBER)))
    else:
        print('No version file found or incorrect data in it!!')

def remove_guard_file(source, target, env):
    """ Remove version increment guard file if present """
    if os.path.exists(VERSION_FOLDER + ".version_no_increment"):
        os.remove(VERSION_FOLDER + ".version_no_increment")

env.AddPostAction("upload", remove_guard_file)

GEN_WEATHER_FILES = False
STOP_BUILD = False

if GEN_WEATHER_FILES:
    # Generated weather icons
    gen_files = [] #generated files
    img_names = [] #image names
    _imgid = 0 #image id
    #source directory
    dir = env.subst("$PROJECT_DIR") + os.sep + "include" + os.sep + "Weather icons" + os.sep+ "pngs"  +os.sep  + "64"  + os.sep  + "c" + os.sep
    #destination directory
    dest_dir = env.subst("$PROJECT_DIR") + os.sep + "include" + os.sep + "Weather icons" + os.sep + "icons" + os.sep
    
    #format the constructor, since the online tool gives a constructor that is not compatible with c++
    #also extract the image name and store it in img_names
    def format_constructor(input_string, imgid):
        input_string = input_string.replace(".header.cf =", "{\n\t")
        input_string = input_string.replace(".header.always_zero =", "")
        input_string = input_string.replace(".header.reserved =", "")
        input_string = input_string.replace(".header.w =", "")
        input_string = input_string.replace(".header.h =", "")
        input_string = input_string.replace(".data_size =","},\n\t")
        input_string = input_string.replace(".data =", "")
        input_string = input_string.replace("const lv_img_dsc_t ", f"//img ID = {imgid}\nconst lv_img_dsc_t w_")
        # find the name of the image
        pattern = r'const\s+lv_img_dsc_t\s+(\w+)\s+='
        img = re.search(pattern, input_string)
        img_names.append(img.group(1))
        return input_string

    #strip dashes and numbers from the image name
    def strip_dash2(input_string):
        result = input_string.replace("- ", "")
        result = result.replace("-.", ".")
        result = result.replace("-", "_")
        result = result.replace(" ", "_")
        result = result.replace("_.", ".")
        return result
    
    #strip dashes that are between valid characters
    def strip_dash(input_string):
        pattern = r'(\S)-(\S)'
        # Use re.sub to replace dashes with underscores between valid characters
        modified_string = re.sub(pattern, r'\1_\2', input_string)
        return modified_string
    
    #remove numbers from the image name
    def remove_numbers(input_string):
    # Create a translation table that maps every digit to None
        table = str.maketrans('', '', string.digits)
    # Use the table to remove all digits from your string
        result = input_string.translate(table)
        result = strip_dash2(result)
        return result
    
    #create the destination directory if it does not exist
    if not os.path.exists(dest_dir):
        os.makedirs(dest_dir)

    for filename in os.listdir(dir):
        src_file = os.path.join(dir, filename)
        gen_files.append(remove_numbers(filename))
        dest_file = os.path.join(dest_dir, remove_numbers(filename))
      
        if os.path.isfile(src_file):
            with open(src_file, 'r') as rf:
                file = rf.read()
                file = strip_dash(file)
                if os.path.isfile(dest_file):
                    os.remove(dest_file)
                with open(dest_file, 'w+') as wf:
                    wf.write(format_constructor(file, _imgid))
                    _imgid += 1
    if gen_files:
        #create the header file weather_icons.h
        #that includes all the generated files
        dest_file = os.path.join(dest_dir, "weather_icons.h")
        #delete the file if it exists than create a new one.
        if os.path.isfile(dest_file):
            os.remove(dest_file)
        with open(dest_file, 'w+') as wf:
            wf.write("""
#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H
///File auto generated by script.py
\n""")
            #include all the generated files
            for file in gen_files:
                wf.write("#include \"" + file + "\"\n")
            wf.write("\n")
            #declare all the images generated
            for img in img_names:
                wf.write("LV_IMG_DECLARE(" + img + ")\n")
            wf.write("\n")
            size = len(img_names)
            wf.write(f'#define WEATHER_ICONS_SIZE {size}\n')


            # img id to icon function
            wf.write("""
///@brief Gets a weather icon based on the img ID
///@param img_id The index of the icon
///@return a const lv_img_dsc_t* to the icon
            """)
            wf.write("const lv_img_dsc_t* get_weather_icons(uint8_t img_id)\n{\n")
            for num, img in enumerate(img_names):
                wf.write(f'\t')
                if num != 0:
                    wf.write("else ")
                wf.write(f'if (img_id == {num})\n')
                wf.write(f'\t\treturn &{img};\n')
            wf.write(f'\treturn NULL;\n')    
            wf.write("}\n")

            # const lv_img_dsc_t* array;
        
            # wf.write(f'const lv_img_dsc_t* weather_icons[{size}] = {"{"}\n');
            # for num, img in enumerate(img_names):
            #     wf.write(f'\t&{img}')
            #     if num < size - 1:
            #         wf.write(",")
            #     wf.write("\n")
            # wf.write("};\n")
            # wf.write("\n")

            wf.write("\n#endif")


if STOP_BUILD:
    raise Exception("stop here")