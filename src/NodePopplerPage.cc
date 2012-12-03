#include <v8.h>
#include <node.h>
#include <node_buffer.h>

#include "NodePopplerDocument.h"
#include "NodePopplerPage.h"

using namespace v8;
using namespace node;

Persistent<FunctionTemplate> NodePopplerPage::constructor_template;
static Persistent<String> width_sym;
static Persistent<String> height_sym;
static Persistent<String> index_sym;

namespace node {

    void NodePopplerPage::Initialize(v8::Handle<v8::Object> target) {
        HandleScope scope;

        width_sym = Persistent<String>::New(String::NewSymbol("width"));
        height_sym = Persistent<String>::New(String::NewSymbol("height"));
        index_sym = Persistent<String>::New(String::NewSymbol("index"));

        Local<FunctionTemplate> t = FunctionTemplate::New(New);
        constructor_template = Persistent<FunctionTemplate>::New(t);
        constructor_template->InstanceTemplate()->SetInternalFieldCount(1);
        constructor_template->SetClassName(String::NewSymbol("PopplerPage"));

        /** Instance methods
         *  static Handle<Value> funcName(const Arguments &args);
         *  NODE_SET_PROTOTYPE_METHOD(constructor_template, "getPageCount", funcName);
         */
        NODE_SET_PROTOTYPE_METHOD(constructor_template, "renderToFile", NodePopplerPage::renderToFile);
        NODE_SET_PROTOTYPE_METHOD(constructor_template, "renderToBuffer", NodePopplerPage::renderToBuffer);
        NODE_SET_PROTOTYPE_METHOD(constructor_template, "findText", NodePopplerPage::findText);
        NODE_SET_PROTOTYPE_METHOD(constructor_template, "addAnnot", NodePopplerPage::addAnnot);
        NODE_SET_PROTOTYPE_METHOD(constructor_template, "deleteAnnots", NodePopplerPage::deleteAnnots);
//
        /** Getters:
         *  static Handle<Value> funcName(Local<String> property, const AccessorInfo& info);
         *  constructor_template->PrototypeTemplate()->SetAccessor(String::NewSymbol("page_count"), funcName);
         */
        constructor_template->InstanceTemplate()->SetAccessor(String::NewSymbol("num"), NodePopplerPage::paramsGetter);
        constructor_template->InstanceTemplate()->SetAccessor(String::NewSymbol("width"), NodePopplerPage::paramsGetter);
        constructor_template->InstanceTemplate()->SetAccessor(String::NewSymbol("height"), NodePopplerPage::paramsGetter);
        constructor_template->InstanceTemplate()->SetAccessor(String::NewSymbol("crop_box"), NodePopplerPage::paramsGetter);
        constructor_template->InstanceTemplate()->SetAccessor(String::NewSymbol("numAnnots"), NodePopplerPage::paramsGetter);

	    /** Class methods
	     * NODE_SET_METHOD(constructor_template->GetFunction(), "GetPageCount", funcName);
	     */
	    // NODE_SET_METHOD(constructor_template->GetFunction(), "pixbufToImage", NodePopplerPage::pixbufToImage);

	    target->Set(String::NewSymbol("PopplerPage"), constructor_template->GetFunction());
    }

    NodePopplerPage::~NodePopplerPage() {
        if (text != NULL) { text->decRefCnt(); }
        if (color != NULL) { delete color; }
        if (!docClosed) { parent->evPageClosed(this); }
    }

    NodePopplerPage::NodePopplerPage(NodePopplerDocument* doc, int32_t pageNum) : ObjectWrap() {
        text = NULL;
        color = NULL;

        pg = doc->doc->getPage(pageNum);
        if (pg && pg->isOk()) {
            parent = doc;
            parent->evPageOpened(this);
            this->doc = doc->doc;
            width = pg->getCropWidth();
            height = pg->getCropHeight();
            color = new AnnotColor(0, 1, 0);
            docClosed = false;
        } else {
            docClosed = true;
        }
    }

    void NodePopplerPage::evDocumentClosed() {
        docClosed = true;
    }

    Handle<Value> NodePopplerPage::New(const Arguments &args) {
        HandleScope scope;
        NodePopplerDocument* doc;
        int32_t pageNum;

        if (args.Length() != 2) {
            return ThrowException(Exception::Error(
                String::New("Two arguments required: (doc: NodePopplerDocument, page: Uint32).")));
        }
        if (!args[1]->IsUint32()) {
            return ThrowException(
                Exception::TypeError(String::New("'page' must be an instance of Uint32.")));
        }
        pageNum = args[1]->ToUint32()->Value();

        if(!args[0]->IsObject()) { // TODO: hasInstance
            return ThrowException(Exception::TypeError(
                String::New("'doc' must be an instance of NodePopplerDocument.")));
        }

        doc = ObjectWrap::Unwrap<NodePopplerDocument>(args[0]->ToObject());
        if (0 >= pageNum || pageNum > doc->doc->getNumPages()) {
            return ThrowException(Exception::Error(String::New(
                "Page number out of bounds.")));
        }

        NodePopplerPage* page = new NodePopplerPage(doc, pageNum);
        if(!page->isOk()) {
            delete page;
            return ThrowException(Exception::Error(String::New("Can't open page.")));;
        }
        page->Wrap(args.This());
        return args.This();
    }

    Handle<Value> NodePopplerPage::paramsGetter(Local<String> property, const AccessorInfo &info) {
        HandleScope scope;

        String::Utf8Value propName(property);
        NodePopplerPage *self = ObjectWrap::Unwrap<NodePopplerPage>(info.This());

        if (strcmp(*propName, "width") == 0) {
            return scope.Close(Number::New(self->getWidth()));
        } else if (strcmp(*propName, "height") == 0) {
            return scope.Close(Number::New(self->getHeight()));
        } else if (strcmp(*propName, "num") == 0) {
            return scope.Close(Uint32::New(self->pg->getNum()));
        } else if (strcmp(*propName, "crop_box") == 0) {
            PDFRectangle *rect = self->pg->getCropBox();
            Local<v8::Object> crop_box = v8::Object::New();

            crop_box->Set(String::NewSymbol("x1"), Number::New(rect->x1));
            crop_box->Set(String::NewSymbol("x2"), Number::New(rect->x2));
            crop_box->Set(String::NewSymbol("y1"), Number::New(rect->y1));
            crop_box->Set(String::NewSymbol("y2"), Number::New(rect->y2));

            return scope.Close(crop_box);
        } else if (strcmp(*propName, "numAnnots") == 0) {
            Annots *annots = self->pg->getAnnots();
            return scope.Close(Uint32::New(annots->getNumAnnots()));
        }
    }

    /**
     * \return Object Relative coors from lower left corner
     */
    Handle<Value> NodePopplerPage::findText(const Arguments &args) {
        HandleScope scope;
        NodePopplerPage* self = ObjectWrap::Unwrap<NodePopplerPage>(args.Holder());
        TextPage *text;
        char *ucs4 = NULL;
        size_t ucs4_len;
        double height, width, xMin = 0, yMin = 0, xMax, yMax;
        PDFRectangle **matches = NULL;
        unsigned int cnt = 0;

        if (self->isDocClosed()) {
            return ThrowException(Exception::Error(String::New(
                "Document closed. You must delete this page")));
        }

        if (args.Length() != 1 && !args[0]->IsString()) {
            return ThrowException(Exception::Error(
                String::New("One argument required: (str: String)")));
        }
        String::Utf8Value str(args[0]);

        iconv_string("UCS-4LE", "UTF-8", *str, *str+strlen(*str)+1, &ucs4, &ucs4_len);
        text = self->getTextPage();
        height = self->pg->getCropHeight();
        width = self->pg->getCropWidth();

        while (text->findText((unsigned int *)ucs4, ucs4_len/4 - 1,
                 gFalse, gTrue, // startAtTop, stopAtBottom
                 gFalse, gFalse, // startAtLast, stopAtLast
                 gFalse, gFalse, // caseSensitive, backwards
                 gFalse, // wholeWord
                 &xMin, &yMin, &xMax, &yMax)) {
            PDFRectangle **t_matches = matches;
            cnt++;
            matches = (PDFRectangle**) realloc(t_matches, sizeof(PDFRectangle*) * cnt);
            matches[cnt-1] = new PDFRectangle(xMin, height - yMax, xMax, height - yMin);
        }
        Local<v8::Array> v8results = v8::Array::New(cnt);
        for (int i = 0; i < cnt; i++) {
            PDFRectangle *match = matches[i];
            Local<v8::Object> v8result = v8::Object::New();
            v8result->Set(String::NewSymbol("x1"), Number::New(match->x1 / width));
            v8result->Set(String::NewSymbol("x2"), Number::New(match->x2 / width));
            v8result->Set(String::NewSymbol("y1"), Number::New(match->y1 / height));
            v8result->Set(String::NewSymbol("y2"), Number::New(match->y2 / height));
            v8results->Set(i, v8result);
            delete match;
        }
        if (ucs4 != NULL) {
            free(ucs4);
        }
        if (matches != NULL) {
            free(matches);
        }
        return scope.Close(v8results);
    }

    /**
     * Delete all annotations
     */
    Handle<Value> NodePopplerPage::deleteAnnots(const Arguments &args) {
        HandleScope scope;
        NodePopplerPage *self = ObjectWrap::Unwrap<NodePopplerPage>(args.Holder());

        Annots *annots = self->pg->getAnnots();

        while (annots->getNumAnnots()) {
            Annot *annot = annots->getAnnot(0);
            annot->invalidateAppearance();
            self->pg->removeAnnot(annot);
        }

        return scope.Close(Null());
    }

    /**
     * Add annotations to page
     *
     * Javascript function
     *
     * \param annot Object or Array of annot objects with fields:
     *  x1 - for lower left corner relative x ord
     *  y1 - for lower left corner relative y ord
     *  x2 - for upper right corner relative x ord
     *  y2 - for upper right corner relative y ord
     */
    Handle<Value> NodePopplerPage::addAnnot(const Arguments &args) {
        HandleScope scope;
        NodePopplerPage* self = ObjectWrap::Unwrap<NodePopplerPage>(args.Holder());

        if (self->isDocClosed()) {
            return ThrowException(Exception::Error(String::New(
                "Document closed. You must delete this page")));
        }

        char *error = NULL;

        if (args.Length() < 1) {
            return ThrowException(Exception::Error(String::New(
                "One argument required: (annot: Object | Array).")));
        }

        if (args[0]->IsArray() &&
                args[0]->ToObject()->Get(String::NewSymbol("length"))->Uint32Value() > 0) {
            self->addAnnot(Handle<v8::Array>::Cast(args[0]), &error);
        } else if (args[0]->IsObject()) {
            Handle<v8::Array> annot = v8::Array::New(1);
            annot->Set(0, args[0]);
            self->addAnnot(annot, &error);
        }

        if (error) {
            Handle<Value> e = Exception::Error(String::New(error));
            delete [] error;
            return ThrowException(e);
        } else {
            return scope.Close(Null());
        }
    }

    /**
     * Add annotations to page
     */
    void NodePopplerPage::addAnnot(Handle<v8::Array> array, char **error) {
        HandleScope scope;

        double x1, y1, x2, y2, x3, y3, x4, y4;
        int len = array->Length();
        AnnotQuadrilaterals::AnnotQuadrilateral **quads = new AnnotQuadrilaterals::AnnotQuadrilateral*[len];

        for (int i = 0; i < array->Length(); i++) {
            parseAnnot(array->Get(i), &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4, error);
            if (*error) {
                for (i--; i >= 0; i--) {
                    delete quads[i];
                }
                delete [] quads;
                return;
            }
            quads[i] = new AnnotQuadrilaterals::AnnotQuadrilateral(x1, y1, x2, y2, x3, y3, x4, y4);
        }

        PDFRectangle *rect = new PDFRectangle(0,0,0,0);
        AnnotQuadrilaterals *aq = new AnnotQuadrilaterals(quads, len);
        AnnotTextMarkup *annot = new AnnotTextMarkup(doc, rect, Annot::typeHighlight, aq);

        annot->setOpacity(.5);
        annot->setColor(color);
        pg->addAnnot(annot);

        delete aq;
        delete rect;
    }

    /**
     * Parse annotation quadrilateral
     */
    void NodePopplerPage::parseAnnot(Handle<Value> rect,
            double *x1, double *y1,
            double *x2, double *y2,
            double *x3, double *y3,
            double *x4, double *y4,
            char **error) {
        HandleScope scope;
        Local<String> x1k = String::NewSymbol("x1");
        Local<String> x2k = String::NewSymbol("x2");
        Local<String> y1k = String::NewSymbol("y1");
        Local<String> y2k = String::NewSymbol("y2");
        if (!rect->IsObject() ||
                !rect->ToObject()->Has(x1k) || !rect->ToObject()->Has(x2k) ||
                !rect->ToObject()->Has(y1k) || !rect->ToObject()->Has(y2k)) {
            char *e = (char*)"Invalid rectangle definition for annotation quadrilateral";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
        } else {
            Handle<Value> x1v = rect->ToObject()->Get(x1k);
            Handle<Value> x2v = rect->ToObject()->Get(x2k);
            Handle<Value> y1v = rect->ToObject()->Get(y1k);
            Handle<Value> y2v = rect->ToObject()->Get(y2k);
            if (!x1v->IsNumber() || !x2v->IsNumber() || !y1v->IsNumber() || !y2v->IsNumber()) {
                char *e = (char*)"Wrong values for rectangle corners definition";
                *error = new char(strlen(e)+1);
                strcpy(*error, e);
            } else {
                *x1 = *x2 = width * x1v->NumberValue();
                *x3 = *x4 = width * x2v->NumberValue();
                *y2 = *y4 = height * y1v->NumberValue();
                *y1 = *y3 = height * y2v->NumberValue();
            }
        }
    }

    /**
     * Displaying page slice to f
     *
     * Caller must free error if it was set
     */
    void NodePopplerPage::display(
            FILE *f, double PPI, Writer wr,
            char *compression, int quality, bool progressive,
            int x, int y, int w, int h, char **error) {
        SplashColor paperColor;
        paperColor[0] = 255;
        paperColor[1] = 255;
        paperColor[2] = 255;
        SplashOutputDev *splashOut = new SplashOutputDev(
            splashModeRGB8,
            4, gFalse,
            paperColor);
        splashOut->startDoc(doc);
        ImgWriter *writer;
        switch (wr) {
            case W_PNG:
                writer = new PNGWriter();
                break;
            case W_JPEG:
                writer = new JpegWriter(quality, progressive);
                break;
            case W_TIFF:
                writer = new TiffWriter();
                ((TiffWriter*)writer)->setCompressionString(compression);
                ((TiffWriter*)writer)->setSplashMode(splashModeRGB8);
        }

        pg->displaySlice(splashOut, PPI, PPI, 0, gFalse, gTrue, x, y, w, h, gFalse);

        SplashBitmap *bitmap = splashOut->getBitmap();
        SplashError e = bitmap->writeImgFile(writer, f, (int)PPI, (int)PPI);
        delete splashOut;
        delete writer;

        if (e) {
            char err[256];
            sprintf(err, "SplashError %d\0", e);
            *error = new char[strlen(err)+1];
            strcpy(*error, err);
        }
    }

    /**
     * Render page to file stream
     *
     * Backend function for \see NodePopplerPage::renderToBuffer and \see NodePopplerPage::renderToFile
     */
    void NodePopplerPage::renderToStream(int argc, Handle<Value> argv[], FILE *f, char **error) {
        HandleScope scope;

        Writer wr;
        char *compression = NULL;
        int quality = 100;
        bool progressive = false;
        double x, y, w, h, PPI, scale, scaledWidth, scaledHeight;
        int sx, sy, sw, sh;

        parseRenderArguments(argv, argc, &wr, &compression, &quality, &progressive, &PPI,
            &x, &y, &w, &h, error);

        if (*error) {
            if (compression) { delete [] compression; }
            return;
        }

        // cap width and height to fit page size
        if (y + h > 1.0) { h = 1.0 - y; }
        if (x + w > 1.0) { w = 1.0 - x; }

        scale = PPI / 72.0;
        scaledWidth = width * scale;
        scaledHeight = height * scale;
        sw = scaledWidth * w;
        sh = scaledHeight * h;
        sx = scaledWidth * x;
        sy = scaledHeight - scaledHeight * y - sh; // converto to bottom related coord

        if ((unsigned long)sh * sw > 100000000L) {
            if (compression) { delete [] compression; }
            char *e = (char*)"Result image is too big";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
            return;
        }

        display(f, PPI, wr, compression, quality, progressive, sx, sy, sw, sh, error);

        if (compression) { delete [] compression; }
    }

    /**
     * Render page to Buffer
     *
     * Javascript function
     *
     * \param method String \see NodePopplerPage::renderToFile
     * \param PPI Number \see NodePopplerPage::renderToFile
     * \param options Object \see NodePopplerPage::renderToFile
     */
    Handle<Value> NodePopplerPage::renderToBuffer(const Arguments &args) {
        HandleScope scope;
        NodePopplerPage* self = ObjectWrap::Unwrap<NodePopplerPage>(args.Holder());

        if (self->isDocClosed()) {
            return ThrowException(Exception::Error(String::New(
                "Document closed. You must delete this page")));
        }

        char *error = NULL;
        FILE *f;
        char *buf = NULL;
        size_t len = 0;

        if (args.Length() < 2) {
            return ThrowException(Exception::Error(String::New(
                "Arguments: (method: String, PPI: Number[, options: Object]")));
        }

        f = open_memstream(&buf, &len);
        if (!f) {
            return ThrowException(Exception::Error(String::New(
                "Can't open output stream")));
        }

        if (args.Length() > 2) {
            Handle<Value> argv[3] = {args[0], args[1], args[2]};
            self->renderToStream(3, argv, f, &error);
        } else {
            Handle<Value> argv[2] = {args[0], args[1]};
            self->renderToStream(2, argv, f, &error);
        }

        fclose(f);

        if (error) {
            Handle<Value> e = Exception::Error(String::New(error));
            delete [] error;
            return ThrowException(e);
        } else {
            Buffer *buffer = Buffer::New(len);
            Handle<v8::Object> out = v8::Object::New();

            memcpy(Buffer::Data(buffer), buf, len);
            free(buf);

            out->Set(String::NewSymbol("type"), String::NewSymbol("buffer"));
            out->Set(String::NewSymbol("format"), args[0]);
            out->Set(String::NewSymbol("data"), buffer->handle_);

            return scope.Close(out);
        }
    }

    /**
     * Render page to file
     *
     * Javascript function
     *
     * \param path String. Path to output file.
     * \param method String with value 'png', 'jpeg' or 'tiff'. Image compression method.
     * \param PPI Number. Pixel per inch value.
     * \param options Object with optional fields:
     *   quality: Integer - defines jpeg quality value (0 - 100) if
     *              image compression method 'jpeg' (default 100)
     *   compression: String - defines tiff compression string if image compression method
     *              is 'tiff' (default NULL).
     *   progressive: Boolean - defines progressive compression for JPEG (default false)
     *   slice: Object - Slice definition in format of object with fields
     *            x: for relative x coordinate of bottom left corner
     *            y: for relative y coordinate of bottom left corner
     *            w: for relative slice width
     *            h: for relative slice height
     *
     * \return Node::Buffer Buffer with rendered image data.
     */
    Handle<Value> NodePopplerPage::renderToFile(const Arguments &args) {
        HandleScope scope;
        NodePopplerPage* self = ObjectWrap::Unwrap<NodePopplerPage>(args.Holder());

        if (self->isDocClosed()) {
            return ThrowException(Exception::Error(String::New(
                "Document closed. You must delete this page")));
        }

        char* cPath = NULL;
        char* error = NULL;
        FILE *f;

        if (args.Length() < 3) {
            return ThrowException(Exception::Error(String::New(
                "Arguments: (path: String, method: String, PPI: Number[, options: Object])"
                )));
        }

        if (!args[0]->IsString()) {
            return ThrowException(Exception::TypeError(String::New(
                "'path' must be an instance of string")));
        } else {
            if (args[0]->ToString()->Utf8Length() > 0) {
                cPath = new char[args[0]->ToString()->Utf8Length() + 1];
                args[0]->ToString()->WriteUtf8(cPath);
            } else {
                return ThrowException(Exception::TypeError(String::New(
                    "'path' can't be empty")));
            }
        }

        f = fopen(cPath, "wb");
        if (!f) {
            delete [] cPath;
            return ThrowException(Exception::Error(String::New(
                "Can't open output file")));
        }

        if (args.Length() == 3) {
            Handle<Value> argv[2] = {args[1], args[2]};
            self->renderToStream(2, argv, f, &error);
        } else {
            Handle<Value> argv[3] = {args[1], args[2], args[3]};
            self->renderToStream(3, argv, f, &error);
        }

        fclose(f);

        if (error) {
            Handle<Value> e = Exception::Error(String::New(error));
            delete [] error;
            unlink(cPath);
            delete [] cPath;
            return ThrowException(e);
        } else {
            Handle<v8::Object> out = v8::Object::New();
            out->Set(String::NewSymbol("type"), String::NewSymbol("file"));
            out->Set(String::NewSymbol("path"), String::New(cPath));
            delete [] cPath;
            return scope.Close(out);
        }
    }

    /**
     * Parses render arguments in order [method, PPI, options]
     *
     * Parses method, PPI and image compression method options (quality for jpeg and compression
     * string for tiff)
     * Caller must free error and compression if was set
     */
    void NodePopplerPage::parseRenderArguments(
            Handle<Value> argv[], int argc,
            Writer *wr, char **compression, int *quality, bool *progressive, double *PPI,
            double *x, double *y, double *w, double *h,
            char **error) {
        HandleScope scope;

        *wr = parseWriter(argv[0], error);
        if (*error) { return; }

        *PPI = parsePPI(argv[1], error);
        if (*error) { return; }

        if (argc == 3) {
            parseWriterOptions(argv[2], *wr, compression, quality, progressive, error);
            if (*error) { return; }
        } else {
            *compression = NULL;
            *quality = 100;
            *progressive = false;
        }

        Handle<String> sk = String::NewSymbol("slice");
        if (argc == 3 && argv[2]->ToObject()->Has(sk)) {
            parseSlice(argv[2]->ToObject()->Get(sk), x, y, w, h, error);
            if (*error) { return; }
        } else {
            *x = *y = 0;
            *w = *h = 1;
        }
    }

    double NodePopplerPage::parsePPI(Handle<Value> PPI, char **error) {
        HandleScope scope;
        if (PPI->IsNumber()) {
            double ppi;
            ppi = PPI->NumberValue();
            if (0 > ppi) {
                char *e = (char*)"'PPI' value must be greater then 0";
                *error = new char[strlen(e)+1];
                strcpy(*error, e);
            } else {
                return ppi;
            }
        } else {
            char *e = (char*)"'PPI' must be an instance of number";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
        }
        return 0;
    }

    /**
     * Determine Writer type from image compression method string
     *
     * Caller must free error if was set
     */
    NodePopplerPage::Writer NodePopplerPage::parseWriter(
            Handle<Value> method, char **error) {
        HandleScope scope;
        if (method->IsString()) {
            String::Utf8Value m(method);
            if (strncmp(*m, "png", 3) == 0) {
                return W_PNG;
            } else if (strncmp(*m, "jpeg", 4) == 0) {
                return W_JPEG;
            } else if (strncmp(*m, "tiff", 4) == 0) {
                return W_TIFF;
            } else {
                char *e = (char*)"Unknown image compression method";
                *error = new char[strlen(e)+1];
                strcpy(*error, e);
                return (Writer) NULL;
            }
        } else {
            char *e = (char*)"'method' must be an instance of String";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
            return (Writer) NULL;
        }
    }

    /**
     * Parse writer options
     *
     * Caller must free compression and error if was set.
     */
    void NodePopplerPage::parseWriterOptions(
            Handle<Value> optionsValue,
            Writer w,
            char **compression, int *quality, bool *progressive,
            char **error) {
        HandleScope scope;

        Local<String> ck = String::NewSymbol("compression");
        Local<String> qk = String::NewSymbol("quality");
        Local<String> pk = String::NewSymbol("progressive");
        Local<v8::Object> options;

        if (!optionsValue->IsObject()) {
            char *e = (char*)"'options' must be an instance of Object";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
            return;
        } else {
            options = optionsValue->ToObject();
        }
        
        switch (w) {
            case W_TIFF:
                if (options->Has(ck)) {
                    Handle<Value> cv = options->Get(ck);
                    if (cv->IsString() && cv->ToString()->Utf8Length() > 0) {
                        Handle<String> cmp = cv->ToString();
                        *compression = new char[cmp->Utf8Length()+1];
                        cmp->WriteUtf8(*compression);
                    } else {
                        compression = NULL;
                    }
                } else {
                    compression = NULL;
                }
                break;
            case W_JPEG:
                if (options->Has(qk)) {
                    Handle<Value> qv = options->Get(qk);
                    if (qv->IsUint32()) {
                        *quality = qv->Uint32Value();
                        if (0 > *quality || *quality > 100) {
                            char *e = (char*)"'quality' not in 0 - 100 interval";
                            *error = new char[strlen(e)+1];
                            strcpy(*error, e);
                        }
                    } else {
                        char *e = (char*)"'quality' must be 0 - 100 interval integer";
                        *error = new char[strlen(e)+1];
                        strcpy(*error, e);
                    }
                } else {
                    *quality = 100;
                }
                if (options->Has(pk)) {
                    Handle<Value> pv = options->Get(pk);
                    if (pv->IsBoolean()) {
                        *progressive = pv->BooleanValue();
                    } else {
                        char *e = (char*)"'progressive' must be a boolean value";
                        *error = new char[strlen(e)+1];
                        strcpy(*error, e);
                    }
                } else {
                    *progressive = false;
                }
                break;
        }
    }

    /**
     * Parse slice values
     *
     * \see NodePopplerPage::renderToFile
     */
    void NodePopplerPage::parseSlice(
            Handle<Value> sliceValue,
            double *x, double *y, double *w, double *h,
            char **error) {
        HandleScope scope;
        Local<v8::Object> slice;
        Local<String> xk = String::NewSymbol("x");
        Local<String> yk = String::NewSymbol("y");
        Local<String> wk = String::NewSymbol("w");
        Local<String> hk = String::NewSymbol("h");

        if (!sliceValue->IsObject()) {
            char *e = (char*)"'slice' must be an instance of Object";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
            return;
        } else {
            slice = sliceValue->ToObject();
        }

        if (slice->Has(xk) && slice->Has(yk) && slice->Has(wk) && slice->Has(hk)) {
            Local<Value> xv = slice->Get(xk);
            Local<Value> yv = slice->Get(yk);
            Local<Value> wv = slice->Get(wk);
            Local<Value> hv = slice->Get(hk);
            if (!xv->IsNumber() || !yv->IsNumber() || !wv->IsNumber() || !hv->IsNumber()) {
                char *e = (char*)"Wrong values for slice";
                *error = new char[strlen(e)+1];
                strcpy(*error, e);
            } else {
                *x = xv->NumberValue();
                *y = yv->NumberValue();
                *w = wv->NumberValue();
                *h = hv->NumberValue();
                if (
                        (0 > *x || *x > 1) ||
                        (0 > *y || *y > 1) ||
                        (0 > *w || *w > 1) ||
                        (0 > *h || *h > 1)) {
                    char *e = (char*)"Slice values not in 0 - 1 interval";
                    *error = new char[strlen(e)+1];
                    strcpy(*error, e);
                }
            }
        } else {
            char *e = (char*)"Not enough values for slice";
            *error = new char[strlen(e)+1];
            strcpy(*error, e);
        }
    }
}