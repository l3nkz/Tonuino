#!/usr/bin/env python3

# Create audio messages necessary for the TonUINO
# Copyright © 2019-2020 Thorsten Voß
#             2020 Till Smejkal
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

from argparse import ArgumentParser
from os.path import join, isfile, isdir
import requests
import base64
import sys


class Mode:
    @staticmethod
    def add_arguments(argparser):
        argparser.add_argument("-i", "--input", type=str, default="audio_tracks.de.txt", dest="input",
                help="The input file with the audio tracks. (default: audio_tracks.de.txt)")
        argparser.add_argument("-o", "--output", type=str, default="sd-card", dest="outdir",
                help="The directory where the generated audio tracks should be saved. (default: sd-card)")
        argparser.add_argument("--lang", type=str, default="de", dest="lang",
                help="The language choice in which the text should be pronounced. (default: de)")

    def __init__(self):
        self._input = None
        self._outdir = None
        self._language = None

    def parse_args(self, args):
        self._input = args.input
        self._outdir = args.outdir
        self._language = args.lang

    def text_to_speech(self, text, lang):
        raise NotImplemented()

    def run(self):
        if not isfile(self._input):
            print("Input file '{}' is not accessible.".format(self._input))
            sys.exit(1)

        if not isdir(self._outdir):
            print("Output directory '{}'' doesn't exist. Please create first.".format(self._outdir))
            sys.exit(1)

        elements = []
        with open(self._input) as f:
            for line in f:
                line = line.strip()

                if len(line) == 0:
                    continue
                if line.startswith("#"):
                    continue

                name, text = line.split('|')
                elements.append((name + ".mp3", text))

        print("Creating texts:")
        errors = 0
        for i, e in enumerate(elements):
            if errors >= 3:
                print("Too many errors. Abort.")
                sys.exit(1)

            name, text = e

            try: 
                print("{}/{} ({}) {}".format(i, len(elements), name, text))
                mp3 = self.text_to_speech(text, self._language)
            except Exception as e:
                print("Encountered on error when converting '{}':\n{}".format(text, e))
                errors += 1
                continue

            target = join(self._outdir, name)
            try:
                with open(target, "wb") as out_file:
                    out_file.write(mp3)
            except IOError as e:
                print("Failed to create file '{}'".format(target))
                errors += 1


class GoogleMode(Mode):

    target_url = "https://texttospeech.googleapis.com/v1beta1/text:synthesize"
    voice_by_lang = {
        "de": { "languageCode": "de-DE", "name": "de-DE-Wavenet-C" },
        "en": { "languageCode": "en-US", "name": "en-US-Wavenet-D" },
    }

    @staticmethod
    def add_arguments(argparser):
        Mode.add_arguments(argparser)

        argparser.add_argument("--google-key", metavar="KEY", type=str, help="The API key for the google text-to-speech account",
                dest="google_key", required=True)
        argparser.set_defaults(create_and_run=GoogleMode.create_and_run)

    @staticmethod
    def create_and_run(args):
        mode = GoogleMode()
        mode.parse_args(args)
        mode.run()

    def __init__(self):
        super().__init__()

        self._key = None

        self._session = requests.Session()

    def parse_args(self, args):
        super().parse_args(args)

        self._key = args.google_key

    def text_to_speech(self, text, lang):
        target_url = GoogleMode.target_url
        params = { "key" : self._key }
        data = { "audioConfig": {
                    "audioEncoding": "MP3",
                    "speakingRate": 1.0,
                    "pitch": 2.0,
                    "sampleRateHertz": 44100,
                    "effectsProfileId": [ "small-bluetooth-speaker-class-device" ]
                },
                "voice": GoogleMode.voice_by_lang[lang],
                "input": { "text": text }
            }
        headers = { "Content-Type" : "application/json", "charset" : "utf-8" }

        r = self._session.post(target_url, headers=headers, params=params, json=data)
        if r.status_code != requests.codes.ok:
            print(r.request.headers)
            print(r.request.url)
            print(r.request.body)
            raise Exception("Request failed with status: {}".format(r.status_code))

        js = r.json()
        if not "audioContent" in js:
            raise Exception("audioContent not in the response json")

        return  base64.b64decode(js["audioContent"])


class AWSMode(Mode):

    voice_by_lang = {
        "de": "Vicki",
        "en": "Joanna",
    }

    @staticmethod
    def add_arguments(argparser):
        Mode.add_arguments(argparser)

        argparser.set_defaults(create_and_run=AWSMode.create_and_run)

    @staticmethod
    def create_and_run(args):
        mode = AWSMode()
        mode.parse_args(args)
        mode.run()

    def __init__(self):
        super().__init__()

    def parse_args(self, args):
        super().parse_args(args)

    def text_to_speech(self, text, lang):
        raise NotImplemented()


class SayMode(Mode):

    voice_by_lang = {
        "de": "Anna",
        "en": "Samantha",
    }

    @staticmethod
    def add_arguments(argparser):
        Mode.add_arguments(argparser)

        argparser.set_defaults(create_and_run=SayMode.create_and_run)

    @staticmethod
    def create_and_run(args):
        mode = SayMode()
        mode.parse_args(args)
        mode.run()

    def __init__(self):
        super().__init__()

    def parse_args(self, args):
        super().parse_args(args)

    def text_to_speech(self, text, lang):
        raise NotImplemented()


if __name__ == "__main__":
    parser = ArgumentParser(description="Convert text to speech")

    subs = parser.add_subparsers()
    GoogleMode.add_arguments(subs.add_parser("google", description="Use Google Text-To-Speech to translate the texts"))

    args = parser.parse_args()
    if "create_and_run" not in args:
        print("You need to specify a valid subcommand.")
        sys.exit(1)

    args.create_and_run(args)
