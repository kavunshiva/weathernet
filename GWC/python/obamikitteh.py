# import pillow
from PIL import Image

# import image pixel color data
pitcha = Image.open("whoa_kitty.jpg")
pitcha_data = list(pitcha.getdata())

# define colors
red = (217, 26, 33)
yellow = (252, 227, 166)
lightBlue = (112, 150, 158)
darkBlue = (0, 51, 76)

# parse pixel color data and modify it
for i in range(len(pitcha_data)):
    intensity = 0;
    for j in range(len(pitcha_data[i])):
        intensity += pitcha_data[i][j]
    # low intensity
    if intensity < 182:
        pitcha_data[i] = darkBlue
    # medium/low intensity
    elif 182 <= intensity < 364:
        pitcha_data[i] = red
    # medium/high intensity
    elif 364 <= intensity < 546:
        pitcha_data[i] = lightBlue
    # high intensity
    elif 546 <= intensity:
        pitcha_data[i] = yellow
    else:
        print("something went wroooong")

# show and save the obamified kitteh!
pitcha.putdata(pitcha_data)
pitcha.show()
pitcha.save("whobamakitteh.jpg", "jpeg")
