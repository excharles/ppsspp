#include "AdhocServerScreen.h"

#include "Common/Net/Resolve.h"
#include "Common/UI/Root.h"
#include "Common/StringUtils.h"

class AdhocServerRow : public UI::LinearLayout {
public:
	AdhocServerRow(std::string *value, const AdhocServerListEntry &entry, UI::LayoutParams *layoutParams = nullptr);

	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = 500; h = 90;
	}

	void Draw(UIContext &dc) override {
		dc.FillRect(dc.GetTheme().itemStyle.background, bounds_);
		if (*value_ == entry_.hostname) {
			// TODO: Make this highlight themable
			dc.FillRect(UI::Drawable(0x30FFFFFF), GetBounds());
		}
		LinearLayout::Draw(dc);
	}

	bool Touch(const TouchInput &input) override {
		using namespace UI;
		if (UI::LinearLayout::Touch(input)) {
			return true;
		}
		if (bounds_.Contains(input.x, input.y) && (input.flags & TouchInputFlags::DOWN)) {
			EventParams e;
			e.v = this;
			OnSelected.Trigger(e);
			return true;
		}
		return false;
	}

	UI::Event OnSelected;
	std::string *value_;
	AdhocServerListEntry entry_;
};

static UI::View *CreateLinkButton(std::string url) {
	using namespace UI;

	ImageID icon = ImageID("I_LINK_OUT_QUESTION");

	if (startsWith(url, "https://discord")) {
		icon = ImageID("I_LOGO_DISCORD");
	}

	Choice *choice = new Choice(icon, new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT));
	choice->OnClick.Add([url](UI::EventParams &) {
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, url);
	});
	return choice;
}

AdhocServerRow::AdhocServerRow(std::string *value, const AdhocServerListEntry &entry, UI::LayoutParams *layoutParams)
	: UI::LinearLayout(ORIENT_HORIZONTAL, new UI::LinearLayoutParams(UI::FILL_PARENT, UI::WRAP_CONTENT, UI::Margins(5.0f, 0.0f))), value_(value), entry_(entry) {
	using namespace UI;

	int number = 0;

	// TEMP HACK: use some other view, like a Choice, themed differently to enable keyboard access to selection.

	LinearLayout *lines = Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(Margins(5.0f, 5.0f))));
	lines->SetSpacing(0.0f);
	lines->Add(new TextView(entry.name));
	lines->Add(new TextView(entry.hostname))->SetTextSize(TextSize::Small)->SetWordWrap();
	lines->Add(new TextView(entry.note))->SetTextSize(TextSize::Tiny)->SetWordWrap();

	Add(new Spacer(0.0f, new LinearLayoutParams(1.0f, Margins(0.0f, 5.0f))));

	if (entry.mode == AdhocDataMode::AemuPostoffice) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		Add(new TextView(di->T("Relay")))->SetTextSize(TextSize::Small);
	}

	Add(CreateLinkButton(entry.community_link));

	Add(new Choice("Use", new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)))->OnClick.Add([this, entry](UI::EventParams &e) {
		e.v = this;
		OnSelected.Trigger(e);
	});
}

AdhocServerScreen::AdhocServerScreen(std::string *value, std::string_view title)
	: UI::PopupScreen(title, T(I18NCat::DIALOG, "OK"), T(I18NCat::DIALOG, "Cancel")), value_(value) {
	resolver_ = std::thread([](AdhocServerScreen *thiz) {
		thiz->ResolverThread();
	}, this);
	editValue_ = *value;

	auto list_to_use = defaultProAdhocServerList;
	downloadedProAdhocServerListMutex.lock();
	if (downloadedProAdhocServerList.size() != 0) {
		list_to_use = downloadedProAdhocServerList;
	}
	downloadedProAdhocServerListMutex.unlock();

	listItems_ = list_to_use;
}

AdhocServerScreen::~AdhocServerScreen() {
	{
		std::unique_lock<std::mutex> guard(resolverLock_);
		resolverState_ = ResolverState::QUIT;
		resolverCond_.notify_one();
	}
	resolver_.join();
}

void AdhocServerScreen::CreatePopupContents(UI::ViewGroup *parent) {
	using namespace UI;
	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	auto n = GetI18NCategory(I18NCat::NETWORKING);

	PopupTextInputChoice *textInputChoice = parent->Add(new PopupTextInputChoice(GetRequesterToken(), &editValue_, n->T("Hostname"), "", 450, screenManager()));

	std::vector<AdhocServerListEntry> entries;

	for (const auto &item : listItems_) {
		entries.push_back(item);
	}

	// Fake entry for localhost.
	AdhocServerListEntry localhostEntry;
	localhostEntry.hostname = "localhost";
	localhostEntry.name = "localhost";
	entries.push_back(localhostEntry);

	parent->Add(new Spacer(5.0f));

	std::vector<std::string> listIP;
	net::GetLocalIP4List(listIP);

	ScrollView *scrollView = new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(1.0f));
	LinearLayout *innerView = new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT));
	innerView->SetSpacing(5.0f);
	for (const auto &label : listIP) {
		// Filter out IP prefixed with "127." and "169.254." also "0." since they can be redundant or unusable
		if (label.find("127.") != 0 && label.find("169.254.") != 0 && label.find("0.") != 0) {
			// Make a fake entry for this local IP address.
			AdhocServerListEntry entry;
			entry.hostname = label;
			entries.push_back(entry);
		}
	}

	for (const auto &entry : entries) {
		AdhocServerRow *row = new AdhocServerRow(&editValue_, entry);
		innerView->Add(row);
		row->OnSelected.Add([this](UI::EventParams &e) {
			std::string value = e.v->Tag();
			if (!value.empty()) {
				editValue_ = value;
				// TODO: Let's change this to an actual button later.
				System_CopyStringToClipboard(value);
			}
		});
		row->SetTag(entry.hostname);
	}

	scrollView->Add(innerView);
	parent->Add(scrollView);
	listIP.clear();
	listIP.shrink_to_fit();

	progressView_ = parent->Add(new NoticeView(NoticeLevel::INFO, n->T("Validating address..."), "", new LinearLayoutParams(Margins(0, 5, 0, 0))));
	progressView_->SetVisibility(UI::V_GONE);
}

void AdhocServerScreen::ResolverThread() {
	std::unique_lock<std::mutex> guard(resolverLock_);

	while (resolverState_ != ResolverState::QUIT) {
		resolverCond_.wait(guard);

		if (resolverState_ == ResolverState::QUEUED) {
			resolverState_ = ResolverState::PROGRESS;

			addrinfo *resolved = nullptr;
			std::string err;
			toResolveResult_ = net::DNSResolve(toResolve_, "80", &resolved, err);
			if (resolved)
				net::DNSResolveFree(resolved);

			resolverState_ = ResolverState::READY;
		}
	}
}

bool AdhocServerScreen::CanComplete(DialogResult result) {
	auto n = GetI18NCategory(I18NCat::NETWORKING);

	if (result != DR_OK)
		return true;

	std::string value = editValue_;
	if (lastResolved_ == value) {
		return true;
	}

	// Currently running.
	if (resolverState_ == ResolverState::PROGRESS)
		return false;

	std::lock_guard<std::mutex> guard(resolverLock_);
	switch (resolverState_) {
	case ResolverState::PROGRESS:
	case ResolverState::QUIT:
		return false;

	case ResolverState::QUEUED:
	case ResolverState::WAITING:
		break;

	case ResolverState::READY:
		if (toResolve_ == value) {
			// Reset the state, nothing there now.
			resolverState_ = ResolverState::WAITING;
			toResolve_.clear();
			lastResolved_ = value;
			lastResolvedResult_ = toResolveResult_;

			if (lastResolvedResult_) {
				progressView_->SetVisibility(UI::V_GONE);
			} else {
				progressView_->SetText(n->T("Invalid IP or hostname"));
				progressView_->SetLevel(NoticeLevel::ERROR);
				progressView_->SetVisibility(UI::V_VISIBLE);
			}
			return true;
		}

		// Throw away that last result, it was for a different value.
		break;
	}

	resolverState_ = ResolverState::QUEUED;
	toResolve_ = value;
	resolverCond_.notify_one();

	progressView_->SetText(n->T("Validating address..."));
	progressView_->SetLevel(NoticeLevel::INFO);
	progressView_->SetVisibility(UI::V_VISIBLE);

	return false;
}

void AdhocServerScreen::OnCompleted(DialogResult result) {
	if (result == DR_OK) {
		*value_ = StripSpaces(editValue_);
	}
}
